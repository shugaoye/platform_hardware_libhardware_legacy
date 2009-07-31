/*
 * Copyright 2008, The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <dirent.h>

#include "hardware_legacy/wifi.h"
#include "libwpa_client/wpa_ctrl.h"

#define LOG_TAG "WifiHW"
#include "cutils/log.h"
#include "cutils/memory.h"
#include "cutils/misc.h"
#include "cutils/properties.h"
#include "private/android_filesystem_config.h"
#ifdef HAVE_LIBC_SYSTEM_PROPERTIES
#define _REALLY_INCLUDE_SYS__SYSTEM_PROPERTIES_H_
#include <sys/_system_properties.h>
#endif
#include <sys/utsname.h>

static struct wpa_ctrl *ctrl_conn;
static struct wpa_ctrl *monitor_conn;

extern int do_dhcp();
extern int ifc_init();
extern void ifc_close();
extern char *dhcp_lasterror();
extern void get_dhcp_info();
extern int init_module(void *, unsigned long, const char *);
extern int delete_module(const char *, unsigned int);

static char iface[PROPERTY_VALUE_MAX];
// TODO: use new ANDROID_SOCKET mechanism, once support for multiple
// sockets is in
#ifndef WIFI_FIRMWARE_LOADER
#define WIFI_FIRMWARE_LOADER		""
#endif
#define WIFI_TEST_INTERFACE             "sta"
#define SYSFS_PATH_MAX                  256
#define SYSFS_CLASS_NET                 "/sys/class/net"
#define MODULE_DEFAULT_DIR              "/lib/modules"
#define PARSE_BUF_SIZE                  4096 /* Should be long enough */
static const char SYS_FS_NET_DIR[]      = "/sys/class/net";
static const char SYS_MOD_NAME_DIR[]    = "device/driver/module";
static const char IFACE_DIR[]           = "/data/system/wpa_supplicant";
static const char FIRMWARE_LOADER[]     = WIFI_FIRMWARE_LOADER;
static const char DRIVER_PROP_NAME[]    = "wlan.driver.status";
static const char SUPPLICANT_NAME[]     = "wpa_supplicant";
static const char SUPP_PROP_NAME[]      = "init.svc.wpa_supplicant";
static const char SUPP_CONFIG_TEMPLATE[]= "/system/etc/wifi/wpa_supplicant.conf";
static const char SUPP_CONFIG_FILE[]    = "/data/misc/wifi/wpa_supplicant.conf";
static const char MODULE_FILE[]         = "/proc/modules";

static int insmod(const char *filename, const char *args)
{
    void *module;
    unsigned int size;
    int ret;

    module = load_file(filename, &size);
    if (!module)
        return -1;

    ret = init_module(module, size, args);

    free(module);

    return ret;
}

static int rmmod(const char *modname)
{
    int ret = -1;
    int maxtry = 10;

    while (maxtry-- > 0) {
        ret = delete_module(modname, O_NONBLOCK | O_EXCL);
        if (ret < 0 && errno == EAGAIN)
            usleep(500000);
        else
            break;
    }

    if (ret != 0)
        LOGD("Unable to unload driver module \"%s\": %s\n",
             modname, strerror(errno));
    return ret;
}

int do_dhcp_request(int *ipaddr, int *gateway, int *mask,
                    int *dns1, int *dns2, int *server, int *lease) {
    /* For test driver, always report success */
    if (strcmp(iface, WIFI_TEST_INTERFACE) == 0)
        return 0;

    if (ifc_init() < 0)
        return -1;

    if (do_dhcp(iface) < 0) {
        ifc_close();
        return -1;
    }
    ifc_close();
    get_dhcp_info(ipaddr, gateway, mask, dns1, dns2, server, lease);
    return 0;
}

const char *get_dhcp_error_string() {
    return dhcp_lasterror();
}

static int get_driver_path( char *path, char *mname,int len) {
    struct utsname un;
    FILE *fd;
    int ret = 0, cnt;
    char *parse_buf;
    char *tptr,*pos;
    parse_buf = malloc(PARSE_BUF_SIZE);
    if (parse_buf ) {
        if (uname(&un) >= 0 ) {
            /* find the path under release dir */
            chdir(MODULE_DEFAULT_DIR);
            chdir(un.release);
            if ((fd = fopen("modules.dep","r")) != NULL)  {
                while(fgets(parse_buf,PARSE_BUF_SIZE-1,fd) != NULL) {
                    pos = strchr(parse_buf,':');
                    *pos = '\0';
                    cnt = strlen(parse_buf);
                    while((cnt >0) && (parse_buf[cnt -1] != '/')) cnt -- ;
                    if (cnt >= 0 && !strcmp(&parse_buf[cnt],mname)) {
                       /* Found */
                       if (len >= (strlen(parse_buf)+strlen(MODULE_DEFAULT_DIR)+strlen(un.release) + 2 )){
                          snprintf(path, len, "%s/%s/%s",MODULE_DEFAULT_DIR,un.release,parse_buf);
                          ret = cnt;
                       } else {
                          LOGE("%s input buffer is too small",__FUNCTION__);
                       }
                       break;
                    }
                }
            } else {
               LOGE("%s Can not find module.dep",__FUNCTION__);
            }
        } else {
            LOGE("%s Can not find uname",__FUNCTION__);
        }
    } else {
        LOGE("%s Wrong out of memory",__FUNCTION__);
    }
    if (parse_buf)
        free(parse_buf);
    return ret;
}

static int get_driver_info() {
    DIR  *netdir;
    struct dirent *de;
    char path[SYSFS_PATH_MAX];
    char link[SYSFS_PATH_MAX];
    int ret = 0;

    if ((netdir = opendir(SYS_FS_NET_DIR)) != NULL) {
        while((de = readdir(netdir))!=NULL) {
            struct dirent **namelist = NULL;
            int cnt;
            if ((!strcmp(de->d_name,".")) || (!strcmp(de->d_name,"..")))
                continue;
            snprintf(path, SYSFS_PATH_MAX,SYSFS_CLASS_NET"/%s/phy80211",de->d_name);
            if (access(path, F_OK)) {
                snprintf(path, SYSFS_PATH_MAX,SYSFS_CLASS_NET"/%s/wireless",de->d_name);
                if (access(path, F_OK))
                    continue;
            }
            snprintf(path,SYSFS_PATH_MAX,SYSFS_CLASS_NET"/%s/%s",de->d_name,SYS_MOD_NAME_DIR);
            cnt = readlink(path, link,SYSFS_PATH_MAX-1);
            if (cnt > 0 ) {
                char *mod = NULL, *mod1 = NULL;
                link[cnt] = '\0';
                strtok(link,"/");
                while((mod = strtok(NULL,"/")) != NULL) {
                    mod1 = mod;
                }
                if (mod1) {
                   property_set("wlan.interface",de->d_name);
                   property_set(DRIVER_PROP_NAME,"ok");
                   property_set("wlan.modname",mod1);
                   snprintf(link,SYSFS_PATH_MAX,"%s.ko",mod1);
                   if ( (cnt = get_driver_path(path, link, SYSFS_PATH_MAX)) > 0)
                   {
                       property_set("wlan.modpath",path);
                   } else {
                       LOGE("%s can not locate wifi driver, you will not be able to reinstall driver",
                             __FUNCTION__);
                   }
                   ret = 1;
                   break;
                }
            } else {
                LOGI("can not find link");
            }
        }
    }
    closedir(netdir);

    return ret;
}

static int check_driver_loaded() {
    char driver_status[PROPERTY_VALUE_MAX];
    int ret;
    if (property_get(DRIVER_PROP_NAME, driver_status, NULL)) {
        if ( strcmp(driver_status, "ok") != 0) {
            return 1;  /* driver loaded */
        } else {
            return 0;  /* Someone has unloaded driver */
        }
    }

    /*
     * This may be the first time we are here and the init
     * script may already installed the driver for us. In
     * that case, we just need to check sys/classs/net/
     * to find the right driver
     */
    if (!(ret = get_driver_info()))
        property_set(DRIVER_PROP_NAME, "unloaded");
    return ret;
}

int wifi_load_driver()
{
    char driver_status[PROPERTY_VALUE_MAX];
    int count = 100; /* wait at most 20 seconds for completion */

    if (check_driver_loaded()) {
        return 0;
    }

    if (!property_get("wlan.modpath",driver_status,NULL))
        return -1;

    if (insmod(driver_status,"") < 0)
        return -1;

    if (strcmp(FIRMWARE_LOADER,"") == 0) {
        usleep(500000);
        property_set(DRIVER_PROP_NAME, "ok");
    }
    else {
        property_set("ctl.start", FIRMWARE_LOADER);
    }
    sched_yield();
    while (count-- > 0) {
        if (property_get(DRIVER_PROP_NAME, driver_status, NULL)) {
            if (strcmp(driver_status, "ok") == 0) {
                /* Update the system prop */
                get_driver_info();
                return 0;
            }
            else if (strcmp(DRIVER_PROP_NAME, "failed") == 0)
                return -1;
        }
        usleep(200000);
    }
    property_set(DRIVER_PROP_NAME, "timeout");
    return -1;
}

int wifi_unload_driver()
{
    char driver_status[PROPERTY_VALUE_MAX];
    int count = 20; /* wait at most 10 seconds for completion */

    if (!property_get("wlan.modname",driver_status,NULL))
        return -1;

    if (rmmod(driver_status) == 0) {
	while (count-- > 0) {
	    if (!check_driver_loaded())
		break;
    	    usleep(500000);
	}
	if (count) {
    	    return 0;
	}
	return -1;
    } else
        return -1;
}

int ensure_config_file_exists()
{
    char buf[2048];
    int srcfd, destfd;
    int nread;

    if (access(SUPP_CONFIG_FILE, R_OK|W_OK) == 0) {
        return 0;
    } else if (errno != ENOENT) {
        LOGE("Cannot access \"%s\": %s", SUPP_CONFIG_FILE, strerror(errno));
        return -1;
    }

    srcfd = open(SUPP_CONFIG_TEMPLATE, O_RDONLY);
    if (srcfd < 0) {
        LOGE("Cannot open \"%s\": %s", SUPP_CONFIG_TEMPLATE, strerror(errno));
        return -1;
    }

    destfd = open(SUPP_CONFIG_FILE, O_CREAT|O_WRONLY, 0660);
    if (destfd < 0) {
        close(srcfd);
        LOGE("Cannot create \"%s\": %s", SUPP_CONFIG_FILE, strerror(errno));
        return -1;
    }

    while ((nread = read(srcfd, buf, sizeof(buf))) != 0) {
        if (nread < 0) {
            LOGE("Error reading \"%s\": %s", SUPP_CONFIG_TEMPLATE, strerror(errno));
            close(srcfd);
            close(destfd);
            unlink(SUPP_CONFIG_FILE);
            return -1;
        }
        write(destfd, buf, nread);
    }

    close(destfd);
    close(srcfd);

    if (chown(SUPP_CONFIG_FILE, AID_SYSTEM, AID_WIFI) < 0) {
        LOGE("Error changing group ownership of %s to %d: %s",
             SUPP_CONFIG_FILE, AID_WIFI, strerror(errno));
        unlink(SUPP_CONFIG_FILE);
        return -1;
    }
    return 0;
}

int wifi_start_supplicant()
{
    char supp_status[PROPERTY_VALUE_MAX] = {'\0'};
    int count = 200; /* wait at most 20 seconds for completion */
#ifdef HAVE_LIBC_SYSTEM_PROPERTIES
    const prop_info *pi;
    unsigned serial = 0;
#endif

    /* Check whether already running */
    if (property_get(SUPP_PROP_NAME, supp_status, NULL)
            && strcmp(supp_status, "running") == 0) {
        return 0;
    }

    /* Before starting the daemon, make sure its config file exists */
    if (ensure_config_file_exists() < 0) {
        LOGE("Wi-Fi will not be enabled");
        return -1;
    }

    /* Clear out any stale socket files that might be left over. */
    wpa_ctrl_cleanup();

#ifdef HAVE_LIBC_SYSTEM_PROPERTIES
    /*
     * Get a reference to the status property, so we can distinguish
     * the case where it goes stopped => running => stopped (i.e.,
     * it start up, but fails right away) from the case in which
     * it starts in the stopped state and never manages to start
     * running at all.
     */
    pi = __system_property_find(SUPP_PROP_NAME);
    if (pi != NULL) {
        serial = pi->serial;
    }
#endif
    property_set("ctl.start", SUPPLICANT_NAME);
    sched_yield();

    while (count-- > 0) {
 #ifdef HAVE_LIBC_SYSTEM_PROPERTIES
        if (pi == NULL) {
            pi = __system_property_find(SUPP_PROP_NAME);
        }
        if (pi != NULL) {
            __system_property_read(pi, NULL, supp_status);
            if (strcmp(supp_status, "running") == 0) {
                return 0;
            } else if (pi->serial != serial &&
                    strcmp(supp_status, "stopped") == 0) {
                return -1;
            }
        }
#else
        if (property_get(SUPP_PROP_NAME, supp_status, NULL)) {
            if (strcmp(supp_status, "running") == 0)
                return 0;
        }
#endif
        usleep(100000);
    }
    return -1;
}

int wifi_stop_supplicant()
{
    char supp_status[PROPERTY_VALUE_MAX] = {'\0'};
    int count = 50; /* wait at most 5 seconds for completion */

    /* Check whether supplicant already stopped */
    if (property_get(SUPP_PROP_NAME, supp_status, NULL)
        && strcmp(supp_status, "stopped") == 0) {
        return 0;
    }

    property_set("ctl.stop", SUPPLICANT_NAME);
    sched_yield();

    while (count-- > 0) {
        if (property_get(SUPP_PROP_NAME, supp_status, NULL)) {
            if (strcmp(supp_status, "stopped") == 0)
                return 0;
        }
        usleep(100000);
    }
    return -1;
}

int wifi_connect_to_supplicant()
{
    char ifname[256];
    char supp_status[PROPERTY_VALUE_MAX] = {'\0'};

    /* Make sure supplicant is running */
    if (!property_get(SUPP_PROP_NAME, supp_status, NULL)
            || strcmp(supp_status, "running") != 0) {
        LOGE("Supplicant not running, cannot connect");
        return -1;
    }

    property_get("wlan.interface", iface, WIFI_TEST_INTERFACE);

    if (access(IFACE_DIR, F_OK) == 0) {
        snprintf(ifname, sizeof(ifname), "%s/%s", IFACE_DIR, iface);
    } else {
        strlcpy(ifname, iface, sizeof(ifname));
    }

    ctrl_conn = wpa_ctrl_open(ifname);
    if (ctrl_conn == NULL) {
        LOGE("Unable to open connection to supplicant on \"%s\": %s",
             ifname, strerror(errno));
        return -1;
    }
    monitor_conn = wpa_ctrl_open(ifname);
    if (monitor_conn == NULL) {
        wpa_ctrl_close(ctrl_conn);
        ctrl_conn = NULL;
        return -1;
    }
    if (wpa_ctrl_attach(monitor_conn) != 0) {
        wpa_ctrl_close(monitor_conn);
        wpa_ctrl_close(ctrl_conn);
        ctrl_conn = monitor_conn = NULL;
        return -1;
    }
    return 0;
}

int wifi_send_command(struct wpa_ctrl *ctrl, const char *cmd, char *reply, size_t *reply_len)
{
    int ret;

    if (ctrl_conn == NULL) {
        LOGV("Not connected to wpa_supplicant - \"%s\" command dropped.\n", cmd);
        return -1;
    }
    ret = wpa_ctrl_request(ctrl, cmd, strlen(cmd), reply, reply_len, NULL);
    if (ret == -2) {
        LOGD("'%s' command timed out.\n", cmd);
        return -2;
    } else if (ret < 0 || strncmp(reply, "FAIL", 4) == 0) {
        return -1;
    }
    if (strncmp(cmd, "PING", 4) == 0) {
        reply[*reply_len] = '\0';
    }

    return 0;
}

int wifi_wait_for_event(char *buf, size_t buflen)
{
    size_t nread = buflen - 1;
    int fd;
    fd_set rfds;
    int result;
    struct timeval tval;
    struct timeval *tptr;
    
    if (monitor_conn == NULL)
        return 0;

    result = wpa_ctrl_recv(monitor_conn, buf, &nread);
    if (result < 0) {
        LOGD("wpa_ctrl_recv failed: %s\n", strerror(errno));
        return -1;
    }
    buf[nread] = '\0';
    /* LOGD("wait_for_event: result=%d nread=%d string=\"%s\"\n", result, nread, buf); */
    /* Check for EOF on the socket */
    if (result == 0 && nread == 0) {
        /* Fabricate an event to pass up */
        LOGD("Received EOF on supplicant socket\n");
        strncpy(buf, WPA_EVENT_TERMINATING " - signal 0 received", buflen-1);
        buf[buflen-1] = '\0';
        return strlen(buf);
    }
    /*
     * Events strings are in the format
     *
     *     <N>CTRL-EVENT-XXX 
     *
     * where N is the message level in numerical form (0=VERBOSE, 1=DEBUG,
     * etc.) and XXX is the event name. The level information is not useful
     * to us, so strip it off.
     */
    if (buf[0] == '<') {
        char *match = strchr(buf, '>');
        if (match != NULL) {
            nread -= (match+1-buf);
            memmove(buf, match+1, nread+1);
        }
    }
    return nread;
}

void wifi_close_supplicant_connection()
{
    if (ctrl_conn != NULL) {
        wpa_ctrl_close(ctrl_conn);
        ctrl_conn = NULL;
    }
    if (monitor_conn != NULL) {
        wpa_ctrl_close(monitor_conn);
        monitor_conn = NULL;
    }
}

int wifi_command(const char *command, char *reply, size_t *reply_len)
{
    return wifi_send_command(ctrl_conn, command, reply, reply_len);
}
