#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stropts.h>
#include <sys/wait.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/extensions/dpms.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#define DRM_FILE "/dev/dri/card0"
#define INPUT_DIR "/dev/input"
#define XBUF_SIZE 200

typedef struct {
	int fd;
	drmModeRes * res;
	uint32_t * dpms_types;
	int count;
} drm;

static void conn_set_dpms(int fd, drmModeConnector * conn,
	int index, int on) {
	if (conn->connection == DRM_MODE_CONNECTED && index >= 0) {
		int res = drmModeConnectorSetProperty(fd, conn->connector_id, conn->props[index],
			on ? DRM_MODE_DPMS_ON : DRM_MODE_DPMS_OFF);
		if (res < 0) {
			fprintf(stderr, "Failed to set DPMS property: %s\n", strerror(-res));
		}
	}
}

static int drm_set_dpms(drm * drm, int on) {
	int res = drmSetMaster(drm->fd);
	if (res < 0) {
		// DRM it owned by another application (e.g. X server)
		return 0;
	} else {
		int i;
		for (i = 0; i < drm->count; i++) {
			drmModeConnector * conn = drmModeGetConnector(drm->fd, drm->res->connectors[i]);
			if (conn) {
				conn_set_dpms(drm->fd, conn, drm->dpms_types[i], on);
				drmModeFreeConnector(conn);
			}
		}
		drmDropMaster(drm->fd);
		return 1;
	}
}

static int conn_get_prop_index(int fd,
	drmModeConnector * conn, const char * name) {
	int i;
	int index = -1;
	for (i = 0; index < 0 && i < conn->count_props; i++) {
		drmModePropertyRes * prop = drmModeGetProperty(fd, conn->props[i]);
		if (prop && !strcmp(prop->name, name)) {
			index = i;
		}
		drmModeFreeProperty(prop);
	}
	return index;
}

static int drm_init(drm * drm) {
	int res = 0;
	drm->fd = open(DRM_FILE, O_RDWR | O_CLOEXEC, 0);
	drm->res = NULL;
	drm->dpms_types = NULL;
	drm->count = 0;
	if (drm->fd >= 0) {
		drmDropMaster(drm->fd);
		drm->res = drmModeGetResources(drm->fd);
		if (drm->res) {
			res = 1;
			int i;
			drm->count = drm->res->count_connectors;
			if (drm->count > 0) {
				drm->dpms_types = malloc(sizeof(uint32_t) * drm->count);
			}
			for (i = 0; i < drm->res->count_connectors; i++) {
				drmModeConnector * conn = drmModeGetConnector(drm->fd, drm->res->connectors[i]);
				if (conn) {
					drm->dpms_types[i] = conn_get_prop_index(drm->fd, conn, "DPMS");
					drmModeFreeConnector(conn);
				} else {
					drm->dpms_types[i] = -1;
				}
			}
		}
	}
	return res;
}

static void drm_release(drm * drm) {
	if (drm->dpms_types) {
		free(drm->dpms_types);
	}
	if (drm->res) {
		drmModeFreeResources(drm->res);
	}
	if (drm->fd >= 0) {
		close(drm->fd);
	}
}

static int xorg_x11_error_handler(Display * display, XErrorEvent * event) {
	return 0;
}

static int xorg_x11_set_dpms(const char * dstr, const char * xauth, int on) {
	int fd[2];
	pipe(fd);
	int pid = fork();
	if (pid == 0) {
		close(2);
		dup(fd[1]);
		close(fd[0]);
		close(fd[1]);
		setenv("LANG", "C", 1);
		setenv("DISPLAY", dstr, 1);
		if (xauth) {
			setenv("XAUTHORITY", xauth, 1);
		} else {
			unsetenv("XAUTHORITY");
		}
		XSetErrorHandler(xorg_x11_error_handler);
		Display * display = XOpenDisplay(NULL);
		if (display) {
			DPMSEnable(display);
			DPMSForceLevel(display, on ? DPMSModeOn : DPMSModeOff);
			XCloseDisplay(display);
			exit(0);
		}
		exit(1);
		return 0;
	} else if (pid > 0) {
		close(fd[1]);
		char buf[XBUF_SIZE];
		int pos = 0;
		int count;
		while ((count = read(fd[0], &buf[pos], 1)) == 1) {
			pos = pos >= XBUF_SIZE - 1 ? XBUF_SIZE - 1 : pos + 1;
		}
		buf[pos] = '\0';
		wait(0);
		close(fd[0]);
		return strstr(buf, "No protocol specified") == NULL;
	} else {
		close(fd[0]);
		close(fd[1]);
		return 0;
	}
}

static int xorg_check_xauth(int pid, const char * dstr, char * auth, int sz) {
	char buf[XBUF_SIZE];
	sprintf(buf, "/proc/%d/environ", pid);
	int fd = open(buf, O_RDONLY);
	int display_matches = 0;
	int xauth_found = 0;
	if (fd >= 0) {
		int pos = 0;
		while (1) {
			int count = read(fd, &buf[pos], 1);
			if (count != 1) {
				buf[pos] = '\0';
			}
			if (buf[pos] == '\0') {
				if (pos > 0) {
					if (strstr(buf, "DISPLAY=") == buf) {
						const char * display = &buf[8];
						if (!strcmp(display, dstr)) {
							display_matches = 1;
							if (xauth_found) {
								break;
							}
						}
					} else if (strstr(buf, "XAUTHORITY=") == buf) {
						const char * xauthority = &buf[11];
						int flen = pos - 11;
						if (flen <= sz) {
							memcpy(auth, xauthority, flen);
							xauth_found = 1;
							if (display_matches) {
								break;
							}
						}
					}
					pos = 0;
				}
				if (count != 1) {
					break;
				}
			} else {
				pos = pos >= XBUF_SIZE - 1 ? XBUF_SIZE - 1 : pos + 1;
			}
		}
		close(fd);
	}
	return display_matches && xauth_found;
}

static int xorg_find_xauth(const char * dstr, char * auth, int sz) {
	char buf[80];
	DIR * dir = opendir("/proc/");
	int res = 0;
	if (dir) {
		struct dirent * entry;
		while (entry = readdir(dir)) {
			if (entry->d_type == DT_DIR && entry->d_name) {
				int pid = atoi(entry->d_name);
				if (pid > 0 && xorg_check_xauth(pid, dstr, auth, sz)) {
					res = 1;
					break;
				}
			}
		}
		closedir(dir);
	}
	return res;
}

static int xorg_pid_set_dpms(int pid, int on) {
	char buf[XBUF_SIZE];
	char display[XBUF_SIZE] = { '\0', };
	char xauth[XBUF_SIZE] = { '\0', };
	sprintf(buf, "/proc/%d/cmdline", pid);
	int fd = open(buf, O_RDONLY);
	int next_xauth = 0;
	if (fd >= 0) {
		int pos = 0;
		while (1) {
			int count = read(fd, &buf[pos], 1);
			if (count != 1) {
				buf[pos] = '\0';
			}
			if (buf[pos] == '\0') {
				if (pos > 0) {
					if (buf[0] == ':' &&
						buf[1] >= '0' &&
						buf[1] <= '9') {
						memcpy(display, buf, pos + 1);
					} else if (!strcmp(buf, "-auth")) {
						next_xauth = 1;
					} else if (next_xauth) {
						next_xauth = 0;
						memcpy(xauth, buf, pos + 1);
					}
					pos = 0;
				}
				if (count != 1) {
					break;
				}
			} else {
				pos = pos >= XBUF_SIZE - 1 ? XBUF_SIZE - 1 : pos + 1;
			}
		}
		close(fd);
		if (display[0]) {
			int res = xorg_x11_set_dpms(display, xauth[0] ? xauth : NULL, on);
			if (!res && xorg_find_xauth(display, xauth, XBUF_SIZE)) {
				res = xorg_x11_set_dpms(display, xauth, on);
			}
			return res;
		}
	}
	return 0;
}

static int xorg_check_pid(const char * pid_str) {
	char buf[80];
	int pid = atoi(pid_str);
	if (pid > 0) {
		sprintf(buf, "/proc/%d/exe", pid);
		ssize_t s = readlink(buf, buf, 79);
		if (s >= 0) {
			buf[s] = '\0';
			if (!strcmp(buf, "/usr/bin/Xorg") ||
				!strcmp(buf, "/usr/lib/Xorg")) {
				return pid;
			}
		}
	}
	return -1;
}

static void xorg_set_dpms(int on) {
	char buf[80];
	DIR * dir = opendir("/proc/");
	if (dir) {
		struct dirent * entry;
		while (entry = readdir(dir)) {
			if (entry->d_type == DT_DIR && entry->d_name) {
				int pid = xorg_check_pid(entry->d_name);
				if (pid > 0) {
					xorg_pid_set_dpms(pid, on);
				}
			}
		}
		closedir(dir);
	}
}

static void set_dpms(drm * drm, int on) {
	if (!drm_set_dpms(drm, on)) {
		xorg_set_dpms(on);
	}
}

static int check_type_and_get_fd(const char * name) {
	char buf[80];
	if (name && strlen(name) < 20 && strstr(name, "event") == name) {
		sprintf(buf, INPUT_DIR "/%s", name);
		int fd = open(buf, O_RDONLY);
		if (fd >= 0) {
			if (ioctl(fd, EVIOCGNAME(sizeof(buf)), buf) != -1 &&
				!strcmp(buf, "Lid Switch")) {
				return fd;
			}
			close(fd);
		}
	}
	return -1;
}

int main() {
	drm drm;
	if (!drm_init(&drm)) {
		fprintf(stderr, "Failed to initialize DRM\n");
		return 1;
	}

	int fd = -1;
	DIR * dir = opendir(INPUT_DIR);
	if (dir) {
		struct dirent * entry;
		while (entry = readdir(dir)) {
			if (entry->d_type == DT_CHR) {
				fd = check_type_and_get_fd(entry->d_name);
				if (fd >= 0) {
					break;
				}
			}
		}
		closedir(dir);
	}
	if (fd <= 0) {
		drm_release(&drm);
		fprintf(stderr, "Failed to open ACPI file\n");
		return 1;
	}

	struct input_event event;
	while (1) {
		int left = sizeof(event);
		char * ptr = (char *) &event;
		while (left > 0) {
			int count = read(fd, ptr, left);
			if (count > 0) {
				ptr += count;
				left -= count;
			} else {
				break;
			}
		}
		if (left != 0) {
			break;
		}
		if (event.type == EV_SW && event.code == SW_LID) {
			set_dpms(&drm, !event.value);
		}
	}
	close(fd);

	drm_release(&drm);
	return 0;
}
