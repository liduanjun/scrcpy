#include "server.h"

#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <SDL2/SDL_timer.h>
#include <SDL2/SDL_platform.h>

#include "adb/adb.h"
#include "util/binary.h"
#include "util/file.h"
#include "util/log.h"
#include "util/net_intr.h"
#include "util/process_intr.h"
#include "util/str.h"

#define SC_SERVER_FILENAME "scrcpy-server"

#define SC_SERVER_PATH_DEFAULT PREFIX "/share/scrcpy/" SC_SERVER_FILENAME
#define SC_DEVICE_SERVER_PATH "/data/local/tmp/scrcpy-server.jar"

#define SC_ADB_PORT_DEFAULT 5555
#define SC_SOCKET_NAME_PREFIX "scrcpy_"

static char *
get_server_path(void) {
#ifdef __WINDOWS__
    const wchar_t *server_path_env = _wgetenv(L"SCRCPY_SERVER_PATH");
#else
    const char *server_path_env = getenv("SCRCPY_SERVER_PATH");
#endif
    if (server_path_env) {
        // if the envvar is set, use it
#ifdef __WINDOWS__
        char *server_path = sc_str_from_wchars(server_path_env);
#else
        char *server_path = strdup(server_path_env);
#endif
        if (!server_path) {
            LOG_OOM();
            return NULL;
        }
        LOGD("Using SCRCPY_SERVER_PATH: %s", server_path);
        return server_path;
    }

#ifndef PORTABLE
    LOGD("Using server: " SC_SERVER_PATH_DEFAULT);
    char *server_path = strdup(SC_SERVER_PATH_DEFAULT);
    if (!server_path) {
        LOG_OOM();
        return NULL;
    }
#else
    char *server_path = sc_file_get_local_path(SC_SERVER_FILENAME);
    if (!server_path) {
        LOGE("Could not get local file path, "
             "using " SC_SERVER_FILENAME " from current directory");
        return strdup(SC_SERVER_FILENAME);
    }

    LOGD("Using server (portable): %s", server_path);
#endif

    return server_path;
}

static void
sc_server_params_destroy(struct sc_server_params *params) {
    // The server stores a copy of the params provided by the user
    free((char *) params->req_serial);
    free((char *) params->crop);
    free((char *) params->video_codec_options);
    free((char *) params->audio_codec_options);
    free((char *) params->video_encoder);
    free((char *) params->audio_encoder);
    free((char *) params->tcpip_dst);
}

static bool
sc_server_params_copy(struct sc_server_params *dst,
                      const struct sc_server_params *src) {
    *dst = *src;

    // The params reference user-allocated memory, so we must copy them to
    // handle them from another thread

#define COPY(FIELD) \
    dst->FIELD = NULL; \
    if (src->FIELD) { \
        dst->FIELD = strdup(src->FIELD); \
        if (!dst->FIELD) { \
            goto error; \
        } \
    }

    COPY(req_serial);
    COPY(crop);
    COPY(video_codec_options);
    COPY(audio_codec_options);
    COPY(video_encoder);
    COPY(audio_encoder);
    COPY(tcpip_dst);
#undef COPY

    return true;

error:
    sc_server_params_destroy(dst);
    return false;
}

static bool
push_server(struct sc_intr *intr, const char *serial) {
    char *server_path = get_server_path();
    if (!server_path) {
        return false;
    }
    if (!sc_file_is_regular(server_path)) {
        LOGE("'%s' does not exist or is not a regular file\n", server_path);
        free(server_path);
        return false;
    }
    bool ok = sc_adb_push(intr, serial, server_path, SC_DEVICE_SERVER_PATH, 0);
    free(server_path);
    return ok;
}

static const char *
log_level_to_server_string(enum sc_log_level level) {
    switch (level) {
        case SC_LOG_LEVEL_VERBOSE:
            return "verbose";
        case SC_LOG_LEVEL_DEBUG:
            return "debug";
        case SC_LOG_LEVEL_INFO:
            return "info";
        case SC_LOG_LEVEL_WARN:
            return "warn";
        case SC_LOG_LEVEL_ERROR:
            return "error";
        default:
            assert(!"unexpected log level");
            return "(unknown)";
    }
}

static bool
sc_server_sleep(struct sc_server *server, sc_tick deadline) {
    sc_mutex_lock(&server->mutex);
    bool timed_out = false;
    while (!server->stopped && !timed_out) {
        timed_out = !sc_cond_timedwait(&server->cond_stopped,
                                       &server->mutex, deadline);
    }
    bool stopped = server->stopped;
    sc_mutex_unlock(&server->mutex);

    return !stopped;
}

static const char *
sc_server_get_codec_name(enum sc_codec codec) {
    switch (codec) {
        case SC_CODEC_H264:
            return "h264";
        case SC_CODEC_H265:
            return "h265";
        case SC_CODEC_AV1:
            return "av1";
        case SC_CODEC_OPUS:
            return "opus";
        case SC_CODEC_AAC:
            return "aac";
        case SC_CODEC_RAW:
            return "raw";
        default:
            return NULL;
    }
}

static sc_pid
execute_server(struct sc_server *server,
               const struct sc_server_params *params) {
    sc_pid pid = SC_PROCESS_NONE;

    const char *serial = server->serial;
    assert(serial);

    const char *cmd[128];
    unsigned count = 0;
    cmd[count++] = sc_adb_get_executable();
    cmd[count++] = "-s";
    cmd[count++] = serial;
    cmd[count++] = "shell";
    cmd[count++] = "CLASSPATH=" SC_DEVICE_SERVER_PATH;
    cmd[count++] = "app_process";

#ifdef SERVER_DEBUGGER
# define SERVER_DEBUGGER_PORT "5005"
    cmd[count++] =
# ifdef SERVER_DEBUGGER_METHOD_NEW
        /* Android 9 and above */
        "-XjdwpProvider:internal -XjdwpOptions:transport=dt_socket,suspend=y,"
        "server=y,address="
# else
        /* Android 8 and below */
        "-agentlib:jdwp=transport=dt_socket,suspend=y,server=y,address="
# endif
            SERVER_DEBUGGER_PORT;
#endif
    cmd[count++] = "/"; // unused
    cmd[count++] = "com.genymobile.scrcpy.Server";
    cmd[count++] = SCRCPY_VERSION;

    unsigned dyn_idx = count; // from there, the strings are allocated
#define ADD_PARAM(fmt, ...) { \
        char *p; \
        if (asprintf(&p, fmt, ## __VA_ARGS__) == -1) { \
            goto end; \
        } \
        cmd[count++] = p; \
    }

    ADD_PARAM("scid=%08x", params->scid);
    ADD_PARAM("log_level=%s", log_level_to_server_string(params->log_level));

    if (!params->video) {
        ADD_PARAM("video=false");
    }
    if (params->video_bit_rate) {
        ADD_PARAM("video_bit_rate=%" PRIu32, params->video_bit_rate);
    }
    if (!params->audio) {
        ADD_PARAM("audio=false");
    }
    if (params->audio_bit_rate) {
        ADD_PARAM("audio_bit_rate=%" PRIu32, params->audio_bit_rate);
    }
    if (params->video_codec != SC_CODEC_H264) {
        ADD_PARAM("video_codec=%s",
                  sc_server_get_codec_name(params->video_codec));
    }
    if (params->audio_codec != SC_CODEC_OPUS) {
        ADD_PARAM("audio_codec=%s",
            sc_server_get_codec_name(params->audio_codec));
    }
    if (params->video_source != SC_VIDEO_SOURCE_DISPLAY){
        assert(params->video_source == SC_VIDEO_SOURCE_CAMERA);
        ADD_PARAM("video_source=camera");
    }
    if (params->audio_source != SC_AUDIO_SOURCE_OUTPUT) {
        assert(params->audio_source == SC_AUDIO_SOURCE_MIC);
        ADD_PARAM("audio_source=mic");
    }
    if (params->max_size) {
        ADD_PARAM("max_size=%" PRIu16, params->max_size);
    }
    if (params->max_fps) {
        ADD_PARAM("max_fps=%" PRIu16, params->max_fps);
    }
    if (params->lock_video_orientation != SC_LOCK_VIDEO_ORIENTATION_UNLOCKED) {
        ADD_PARAM("lock_video_orientation=%" PRIi8,
                  params->lock_video_orientation);
    }
    if (server->tunnel.forward) {
        ADD_PARAM("tunnel_forward=true");
    }
    if (params->crop) {
        ADD_PARAM("crop=%s", params->crop);
    }
    if (!params->control) {
        // By default, control is true
        ADD_PARAM("control=false");
    }
    if (params->video_source == SC_VIDEO_SOURCE_DISPLAY && params->display_id) {
        ADD_PARAM("display_id=%" PRIu32, params->display_id);
    }
    if (params->video_source == SC_VIDEO_SOURCE_CAMERA && params->camera_id) {
        ADD_PARAM("camera_id=%s", params->camera_id);
    }
    if (params->video_source == SC_VIDEO_SOURCE_CAMERA && params->camera_position != SC_CAMERA_POSITION_ALL) {
        switch (params->camera_position) {
            case SC_CAMERA_POSITION_FRONT:
                ADD_PARAM("camera_position=front");
                break;
            case SC_CAMERA_POSITION_BACK:
                ADD_PARAM("camera_position=back");
                break;
            case SC_CAMERA_POSITION_EXTERNAL:
                ADD_PARAM("camera_position=external");
                break;
        }
    }
    if (params->show_touches) {
        ADD_PARAM("show_touches=true");
    }
    if (params->stay_awake) {
        ADD_PARAM("stay_awake=true");
    }
    if (params->video_codec_options) {
        ADD_PARAM("video_codec_options=%s", params->video_codec_options);
    }
    if (params->audio_codec_options) {
        ADD_PARAM("audio_codec_options=%s", params->audio_codec_options);
    }
    if (params->video_encoder) {
        ADD_PARAM("video_encoder=%s", params->video_encoder);
    }
    if (params->audio_encoder) {
        ADD_PARAM("audio_encoder=%s", params->audio_encoder);
    }
    if (params->power_off_on_close) {
        ADD_PARAM("power_off_on_close=true");
    }
    if (!params->clipboard_autosync) {
        // By default, clipboard_autosync is true
        ADD_PARAM("clipboard_autosync=false");
    }
    if (!params->downsize_on_error) {
        // By default, downsize_on_error is true
        ADD_PARAM("downsize_on_error=false");
    }
    if (!params->cleanup) {
        // By default, cleanup is true
        ADD_PARAM("cleanup=false");
    }
    if (!params->power_on) {
        // By default, power_on is true
        ADD_PARAM("power_on=false");
    }
    if (params->list_encoders) {
        ADD_PARAM("list_encoders=true");
    }
    if (params->list_displays) {
        ADD_PARAM("list_displays=true");
    }
    if (params->list_cameras) {
        ADD_PARAM("list_cameras=true");
    }

#undef ADD_PARAM

    cmd[count++] = NULL;

#ifdef SERVER_DEBUGGER
    LOGI("Server debugger waiting for a client on device port "
         SERVER_DEBUGGER_PORT "...");
    // From the computer, run
    //     adb forward tcp:5005 tcp:5005
    // Then, from Android Studio: Run > Debug > Edit configurations...
    // On the left, click on '+', "Remote", with:
    //     Host: localhost
    //     Port: 5005
    // Then click on "Debug"
#endif
    // Inherit both stdout and stderr (all server logs are printed to stdout)
    pid = sc_adb_execute(cmd, 0);

end:
    for (unsigned i = dyn_idx; i < count; ++i) {
        free((char *) cmd[i]);
    }

    return pid;
}

static bool
connect_and_read_byte(struct sc_intr *intr, sc_socket socket,
                      uint32_t tunnel_host, uint16_t tunnel_port) {
    bool ok = net_connect_intr(intr, socket, tunnel_host, tunnel_port);
    if (!ok) {
        return false;
    }

    char byte;
    // the connection may succeed even if the server behind the "adb tunnel"
    // is not listening, so read one byte to detect a working connection
    if (net_recv_intr(intr, socket, &byte, 1) != 1) {
        // the server is not listening yet behind the adb tunnel
        return false;
    }

    return true;
}

static sc_socket
connect_to_server(struct sc_server *server, unsigned attempts, sc_tick delay,
                  uint32_t host, uint16_t port) {
    do {
        LOGD("Remaining connection attempts: %u", attempts);
        sc_socket socket = net_socket();
        if (socket != SC_SOCKET_NONE) {
            bool ok = connect_and_read_byte(&server->intr, socket, host, port);
            if (ok) {
                // it worked!
                return socket;
            }

            net_close(socket);
        }

        if (sc_intr_is_interrupted(&server->intr)) {
            // Stop immediately
            break;
        }

        if (attempts) {
            sc_tick deadline = sc_tick_now() + delay;
            bool ok = sc_server_sleep(server, deadline);
            if (!ok) {
                LOGI("Connection attempt stopped");
                break;
            }
        }
    } while (--attempts);
    return SC_SOCKET_NONE;
}

bool
sc_server_init(struct sc_server *server, const struct sc_server_params *params,
              const struct sc_server_callbacks *cbs, void *cbs_userdata) {
    bool ok = sc_server_params_copy(&server->params, params);
    if (!ok) {
        LOG_OOM();
        return false;
    }

    ok = sc_mutex_init(&server->mutex);
    if (!ok) {
        sc_server_params_destroy(&server->params);
        return false;
    }

    ok = sc_cond_init(&server->cond_stopped);
    if (!ok) {
        sc_mutex_destroy(&server->mutex);
        sc_server_params_destroy(&server->params);
        return false;
    }

    ok = sc_intr_init(&server->intr);
    if (!ok) {
        sc_cond_destroy(&server->cond_stopped);
        sc_mutex_destroy(&server->mutex);
        sc_server_params_destroy(&server->params);
        return false;
    }

    server->serial = NULL;
    server->device_socket_name = NULL;
    server->stopped = false;

    server->video_socket = SC_SOCKET_NONE;
    server->audio_socket = SC_SOCKET_NONE;
    server->control_socket = SC_SOCKET_NONE;

    sc_adb_tunnel_init(&server->tunnel);

    assert(cbs);
    assert(cbs->on_connection_failed);
    assert(cbs->on_connected);
    assert(cbs->on_disconnected);

    server->cbs = cbs;
    server->cbs_userdata = cbs_userdata;

    return true;
}

static bool
device_read_info(struct sc_intr *intr, sc_socket device_socket,
                 struct sc_server_info *info) {
    unsigned char buf[SC_DEVICE_NAME_FIELD_LENGTH];
    ssize_t r = net_recv_all_intr(intr, device_socket, buf, sizeof(buf));
    if (r < SC_DEVICE_NAME_FIELD_LENGTH) {
        LOGE("Could not retrieve device information");
        return false;
    }
    // in case the client sends garbage
    buf[SC_DEVICE_NAME_FIELD_LENGTH - 1] = '\0';
    memcpy(info->device_name, (char *) buf, sizeof(info->device_name));

    return true;
}

static bool
sc_server_connect_to(struct sc_server *server, struct sc_server_info *info) {
    struct sc_adb_tunnel *tunnel = &server->tunnel;

    assert(tunnel->enabled);

    const char *serial = server->serial;
    assert(serial);

    bool video = server->params.video;
    bool audio = server->params.audio;
    bool control = server->params.control;

    sc_socket video_socket = SC_SOCKET_NONE;
    sc_socket audio_socket = SC_SOCKET_NONE;
    sc_socket control_socket = SC_SOCKET_NONE;
    if (!tunnel->forward) {
        if (video) {
            video_socket =
                net_accept_intr(&server->intr, tunnel->server_socket);
            if (video_socket == SC_SOCKET_NONE) {
                goto fail;
            }
        }

        if (audio) {
            audio_socket =
                net_accept_intr(&server->intr, tunnel->server_socket);
            if (audio_socket == SC_SOCKET_NONE) {
                goto fail;
            }
        }

        if (control) {
            control_socket =
                net_accept_intr(&server->intr, tunnel->server_socket);
            if (control_socket == SC_SOCKET_NONE) {
                goto fail;
            }
        }
    } else {
        uint32_t tunnel_host = server->params.tunnel_host;
        if (!tunnel_host) {
            tunnel_host = IPV4_LOCALHOST;
        }

        uint16_t tunnel_port = server->params.tunnel_port;
        if (!tunnel_port) {
            tunnel_port = tunnel->local_port;
        }

        unsigned attempts = 100;
        sc_tick delay = SC_TICK_FROM_MS(100);
        sc_socket first_socket = connect_to_server(server, attempts, delay,
                                                   tunnel_host, tunnel_port);
        if (first_socket == SC_SOCKET_NONE) {
            goto fail;
        }

        if (video) {
            video_socket = first_socket;
        }

        if (audio) {
            if (!video) {
                audio_socket = first_socket;
            } else {
                audio_socket = net_socket();
                if (audio_socket == SC_SOCKET_NONE) {
                    goto fail;
                }
                bool ok = net_connect_intr(&server->intr, audio_socket,
                                           tunnel_host, tunnel_port);
                if (!ok) {
                    goto fail;
                }
            }
        }

        if (control) {
            if (!video && !audio) {
                control_socket = first_socket;
            } else {
                control_socket = net_socket();
                if (control_socket == SC_SOCKET_NONE) {
                    goto fail;
                }
                bool ok = net_connect_intr(&server->intr, control_socket,
                                           tunnel_host, tunnel_port);
                if (!ok) {
                    goto fail;
                }
            }
        }
    }

    // we don't need the adb tunnel anymore
    sc_adb_tunnel_close(tunnel, &server->intr, serial,
                        server->device_socket_name);

    sc_socket first_socket = video ? video_socket
                           : audio ? audio_socket
                                   : control_socket;

    // The sockets will be closed on stop if device_read_info() fails
    bool ok = device_read_info(&server->intr, first_socket, info);
    if (!ok) {
        goto fail;
    }

    assert(!video || video_socket != SC_SOCKET_NONE);
    assert(!audio || audio_socket != SC_SOCKET_NONE);
    assert(!control || control_socket != SC_SOCKET_NONE);

    server->video_socket = video_socket;
    server->audio_socket = audio_socket;
    server->control_socket = control_socket;

    return true;

fail:
    if (video_socket != SC_SOCKET_NONE) {
        if (!net_close(video_socket)) {
            LOGW("Could not close video socket");
        }
    }

    if (audio_socket != SC_SOCKET_NONE) {
        if (!net_close(audio_socket)) {
            LOGW("Could not close audio socket");
        }
    }

    if (control_socket != SC_SOCKET_NONE) {
        if (!net_close(control_socket)) {
            LOGW("Could not close control socket");
        }
    }

    if (tunnel->enabled) {
        // Always leave this function with tunnel disabled
        sc_adb_tunnel_close(tunnel, &server->intr, serial,
                            server->device_socket_name);
    }

    return false;
}

static void
sc_server_on_terminated(void *userdata) {
    struct sc_server *server = userdata;

    // If the server process dies before connecting to the server socket,
    // then the client will be stuck forever on accept(). To avoid the problem,
    // wake up the accept() call (or any other) when the server dies, like on
    // stop() (it is safe to call interrupt() twice).
    sc_intr_interrupt(&server->intr);

    server->cbs->on_disconnected(server, server->cbs_userdata);

    LOGD("Server terminated");
}

static uint16_t
get_adb_tcp_port(struct sc_server *server, const char *serial) {
    struct sc_intr *intr = &server->intr;

    char *current_port =
        sc_adb_getprop(intr, serial, "service.adb.tcp.port", SC_ADB_SILENT);
    if (!current_port) {
        return 0;
    }

    long value;
    bool ok = sc_str_parse_integer(current_port, &value);
    free(current_port);
    if (!ok) {
        return 0;
    }

    if (value < 0 || value > 0xFFFF) {
        return 0;
    }

    return value;
}

static bool
wait_tcpip_mode_enabled(struct sc_server *server, const char *serial,
                        uint16_t expected_port, unsigned attempts,
                        sc_tick delay) {
    uint16_t adb_port = get_adb_tcp_port(server, serial);
    if (adb_port == expected_port) {
        return true;
    }

    // Only print this log if TCP/IP is not enabled
    LOGI("Waiting for TCP/IP mode enabled...");

    do {
        sc_tick deadline = sc_tick_now() + delay;
        if (!sc_server_sleep(server, deadline)) {
            LOGI("TCP/IP mode waiting interrupted");
            return false;
        }

        adb_port = get_adb_tcp_port(server, serial);
        if (adb_port == expected_port) {
            return true;
        }
    } while (--attempts);
    return false;
}

static char *
append_port(const char *ip, uint16_t port) {
    char *ip_port;
    int ret = asprintf(&ip_port, "%s:%" PRIu16, ip, port);
    if (ret == -1) {
        LOG_OOM();
        return NULL;
    }

    return ip_port;
}

static char *
sc_server_switch_to_tcpip(struct sc_server *server, const char *serial) {
    assert(serial);

    struct sc_intr *intr = &server->intr;

    LOGI("Switching device %s to TCP/IP...", serial);

    char *ip = sc_adb_get_device_ip(intr, serial, 0);
    if (!ip) {
        LOGE("Device IP not found");
        return NULL;
    }

    uint16_t adb_port = get_adb_tcp_port(server, serial);
    if (adb_port) {
        LOGI("TCP/IP mode already enabled on port %" PRIu16, adb_port);
    } else {
        LOGI("Enabling TCP/IP mode on port " SC_STR(SC_ADB_PORT_DEFAULT) "...");

        bool ok = sc_adb_tcpip(intr, serial, SC_ADB_PORT_DEFAULT,
                               SC_ADB_NO_STDOUT);
        if (!ok) {
            LOGE("Could not restart adbd in TCP/IP mode");
            free(ip);
            return NULL;
        }

        unsigned attempts = 40;
        sc_tick delay = SC_TICK_FROM_MS(250);
        ok = wait_tcpip_mode_enabled(server, serial, SC_ADB_PORT_DEFAULT,
                                     attempts, delay);
        if (!ok) {
            free(ip);
            return NULL;
        }

        adb_port = SC_ADB_PORT_DEFAULT;
        LOGI("TCP/IP mode enabled on port " SC_STR(SC_ADB_PORT_DEFAULT));
    }

    char *ip_port = append_port(ip, adb_port);
    free(ip);
    return ip_port;
}

static bool
sc_server_connect_to_tcpip(struct sc_server *server, const char *ip_port) {
    struct sc_intr *intr = &server->intr;

    // Error expected if not connected, do not report any error
    sc_adb_disconnect(intr, ip_port, SC_ADB_SILENT);

    LOGI("Connecting to %s...", ip_port);

    bool ok = sc_adb_connect(intr, ip_port, 0);
    if (!ok) {
        LOGE("Could not connect to %s", ip_port);
        return false;
    }

    LOGI("Connected to %s", ip_port);
    return true;
}

static bool
sc_server_configure_tcpip_known_address(struct sc_server *server,
                                        const char *addr) {
    // Append ":5555" if no port is present
    bool contains_port = strchr(addr, ':');
    char *ip_port = contains_port ? strdup(addr)
                                  : append_port(addr, SC_ADB_PORT_DEFAULT);
    if (!ip_port) {
        LOG_OOM();
        return false;
    }

    server->serial = ip_port;
    return sc_server_connect_to_tcpip(server, ip_port);
}

static bool
sc_server_configure_tcpip_unknown_address(struct sc_server *server,
                                          const char *serial) {
    bool is_already_tcpip =
        sc_adb_device_get_type(serial) == SC_ADB_DEVICE_TYPE_TCPIP;
    if (is_already_tcpip) {
        // Nothing to do
        LOGI("Device already connected via TCP/IP: %s", serial);
        server->serial = strdup(serial);
        if (!server->serial) {
            LOG_OOM();
            return false;
        }
        return true;
    }

    char *ip_port = sc_server_switch_to_tcpip(server, serial);
    if (!ip_port) {
        return false;
    }

    server->serial = ip_port;
    return sc_server_connect_to_tcpip(server, ip_port);
}

static void
sc_server_kill_adb_if_requested(struct sc_server *server) {
    if (server->params.kill_adb_on_close) {
        LOGI("Killing adb server...");
        unsigned flags = SC_ADB_NO_STDOUT | SC_ADB_NO_STDERR | SC_ADB_NO_LOGERR;
        sc_adb_kill_server(&server->intr, flags);
    }
}

static int
run_server(void *data) {
    struct sc_server *server = data;

    const struct sc_server_params *params = &server->params;

    // Execute "adb start-server" before "adb devices" so that daemon starting
    // output/errors is correctly printed in the console ("adb devices" output
    // is parsed, so it is not output)
    bool ok = sc_adb_start_server(&server->intr, 0);
    if (!ok) {
        LOGE("Could not start adb server");
        goto error_connection_failed;
    }

    // params->tcpip_dst implies params->tcpip
    assert(!params->tcpip_dst || params->tcpip);

    // If tcpip_dst parameter is given, then it must connect to this address.
    // Therefore, the device is unknown, so serial is meaningless at this point.
    assert(!params->req_serial || !params->tcpip_dst);

    // A device must be selected via a serial in all cases except when --tcpip=
    // is called with a parameter (in that case, the device may initially not
    // exist, and scrcpy will execute "adb connect").
    bool need_initial_serial = !params->tcpip_dst;

    if (need_initial_serial) {
        // At most one of the 3 following parameters may be set
        assert(!!params->req_serial
               + params->select_usb
               + params->select_tcpip <= 1);

        struct sc_adb_device_selector selector;
        if (params->req_serial) {
            selector.type = SC_ADB_DEVICE_SELECT_SERIAL;
            selector.serial = params->req_serial;
        } else if (params->select_usb) {
            selector.type = SC_ADB_DEVICE_SELECT_USB;
        } else if (params->select_tcpip) {
            selector.type = SC_ADB_DEVICE_SELECT_TCPIP;
        } else {
            // No explicit selection, check $ANDROID_SERIAL
            const char *env_serial = getenv("ANDROID_SERIAL");
            if (env_serial) {
                LOGI("Using ANDROID_SERIAL: %s", env_serial);
                selector.type = SC_ADB_DEVICE_SELECT_SERIAL;
                selector.serial = env_serial;
            } else {
                selector.type = SC_ADB_DEVICE_SELECT_ALL;
            }
        }
        struct sc_adb_device device;
        ok = sc_adb_select_device(&server->intr, &selector, 0, &device);
        if (!ok) {
            goto error_connection_failed;
        }

        if (params->tcpip) {
            assert(!params->tcpip_dst);
            ok = sc_server_configure_tcpip_unknown_address(server,
                                                           device.serial);
            sc_adb_device_destroy(&device);
            if (!ok) {
                goto error_connection_failed;
            }
            assert(server->serial);
        } else {
            // "move" the device.serial without copy
            server->serial = device.serial;
            // the serial must not be freed by the destructor
            device.serial = NULL;
            sc_adb_device_destroy(&device);
        }
    } else {
        ok = sc_server_configure_tcpip_known_address(server, params->tcpip_dst);
        if (!ok) {
            goto error_connection_failed;
        }
    }

    const char *serial = server->serial;
    assert(serial);
    LOGD("Device serial: %s", serial);

    ok = push_server(&server->intr, serial);
    if (!ok) {
        goto error_connection_failed;
    }

    // If --list-* is passed, then the server just prints the requested data
    // then exits.
    if (params->list_encoders || params->list_displays || params->list_cameras) {
        sc_pid pid = execute_server(server, params);
        if (pid == SC_PROCESS_NONE) {
            goto error_connection_failed;
        }
        sc_process_wait(pid, NULL); // ignore exit code
        sc_process_close(pid);
        // Wake up await_for_server()
        server->cbs->on_connected(server, server->cbs_userdata);
        return 0;
    }

    int r = asprintf(&server->device_socket_name, SC_SOCKET_NAME_PREFIX "%08x",
                     params->scid);
    if (r == -1) {
        LOG_OOM();
        goto error_connection_failed;
    }
    assert(r == sizeof(SC_SOCKET_NAME_PREFIX) - 1 + 8);
    assert(server->device_socket_name);

    ok = sc_adb_tunnel_open(&server->tunnel, &server->intr, serial,
                            server->device_socket_name, params->port_range,
                            params->force_adb_forward);
    if (!ok) {
        goto error_connection_failed;
    }

    // server will connect to our server socket
    sc_pid pid = execute_server(server, params);
    if (pid == SC_PROCESS_NONE) {
        sc_adb_tunnel_close(&server->tunnel, &server->intr, serial,
                            server->device_socket_name);
        goto error_connection_failed;
    }

    static const struct sc_process_listener listener = {
        .on_terminated = sc_server_on_terminated,
    };
    struct sc_process_observer observer;
    ok = sc_process_observer_init(&observer, pid, &listener, server);
    if (!ok) {
        sc_process_terminate(pid);
        sc_process_wait(pid, true); // ignore exit code
        sc_adb_tunnel_close(&server->tunnel, &server->intr, serial,
                            server->device_socket_name);
        goto error_connection_failed;
    }

    ok = sc_server_connect_to(server, &server->info);
    // The tunnel is always closed by server_connect_to()
    if (!ok) {
        sc_process_terminate(pid);
        sc_process_wait(pid, true); // ignore exit code
        sc_process_observer_join(&observer);
        sc_process_observer_destroy(&observer);
        goto error_connection_failed;
    }

    // Now connected
    server->cbs->on_connected(server, server->cbs_userdata);

    // Wait for server_stop()
    sc_mutex_lock(&server->mutex);
    while (!server->stopped) {
        sc_cond_wait(&server->cond_stopped, &server->mutex);
    }
    sc_mutex_unlock(&server->mutex);

    // Interrupt sockets to wake up socket blocking calls on the server

    if (server->video_socket != SC_SOCKET_NONE) {
        // There is no video_socket if --no-video is set
        net_interrupt(server->video_socket);
    }

    if (server->audio_socket != SC_SOCKET_NONE) {
        // There is no audio_socket if --no-audio is set
        net_interrupt(server->audio_socket);
    }

    if (server->control_socket != SC_SOCKET_NONE) {
        // There is no control_socket if --no-control is set
        net_interrupt(server->control_socket);
    }

    // Give some delay for the server to terminate properly
#define WATCHDOG_DELAY SC_TICK_FROM_SEC(1)
    sc_tick deadline = sc_tick_now() + WATCHDOG_DELAY;
    bool terminated = sc_process_observer_timedwait(&observer, deadline);

    // After this delay, kill the server if it's not dead already.
    // On some devices, closing the sockets is not sufficient to wake up the
    // blocking calls while the device is asleep.
    if (!terminated) {
        // The process may have terminated since the check, but it is not
        // reaped (closed) yet, so its PID is still valid, and it is ok to call
        // sc_process_terminate() even in that case.
        LOGW("Killing the server...");
        sc_process_terminate(pid);
    }

    sc_process_observer_join(&observer);
    sc_process_observer_destroy(&observer);

    sc_process_close(pid);

    sc_server_kill_adb_if_requested(server);

    return 0;

error_connection_failed:
    sc_server_kill_adb_if_requested(server);
    server->cbs->on_connection_failed(server, server->cbs_userdata);
    return -1;
}

bool
sc_server_start(struct sc_server *server) {
    bool ok =
        sc_thread_create(&server->thread, run_server, "scrcpy-server", server);
    if (!ok) {
        LOGE("Could not create server thread");
        return false;
    }

    return true;
}

void
sc_server_stop(struct sc_server *server) {
    sc_mutex_lock(&server->mutex);
    server->stopped = true;
    sc_cond_signal(&server->cond_stopped);
    sc_intr_interrupt(&server->intr);
    sc_mutex_unlock(&server->mutex);
}

void
sc_server_join(struct sc_server *server) {
    sc_thread_join(&server->thread, NULL);
}

void
sc_server_destroy(struct sc_server *server) {
    if (server->video_socket != SC_SOCKET_NONE) {
        net_close(server->video_socket);
    }
    if (server->audio_socket != SC_SOCKET_NONE) {
        net_close(server->audio_socket);
    }
    if (server->control_socket != SC_SOCKET_NONE) {
        net_close(server->control_socket);
    }

    free(server->serial);
    free(server->device_socket_name);
    sc_server_params_destroy(&server->params);
    sc_intr_destroy(&server->intr);
    sc_cond_destroy(&server->cond_stopped);
    sc_mutex_destroy(&server->mutex);
}
