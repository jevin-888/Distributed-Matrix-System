#include "node/MediaPlayer.h"
#include "common/Logger.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <dlfcn.h>
#include <filesystem>
#include <arpa/inet.h>
#include <fcntl.h>
#include <fstream>
#include <curl/curl.h>
#include <linux/videodev2.h>
#include <iomanip>
#include <limits>
#include <poll.h>
#include <sstream>
#include <string>
#include <set>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace dms {
namespace {

struct GError {
    uint32_t domain;
    int code;
    char* message;
};

// Public GStreamer ABI prefix used to inspect GstMessage::type while keeping
// the production binary independent from the GStreamer development package.
struct GstMiniObjectPrefix {
    uintptr_t type;
    int refcount;
    int lockstate;
    unsigned int flags;
    void* copy;
    void* dispose;
    void* free;
    unsigned int privUint;
    void* privPointer;
};

struct GstMessagePrefix {
    GstMiniObjectPrefix miniObject;
    uint32_t type;
};

constexpr int GST_STATE_NULL = 1;
constexpr int GST_STATE_PAUSED = 3;
constexpr int GST_STATE_PLAYING = 4;
constexpr int GST_STATE_CHANGE_FAILURE = 0;
constexpr int GST_STATE_CHANGE_ASYNC = 2;
constexpr uint32_t GST_MESSAGE_EOS = 1U << 0U;
constexpr uint32_t GST_MESSAGE_ERROR = 1U << 1U;
constexpr uint64_t GST_CLOCK_TIME_NONE = std::numeric_limits<uint64_t>::max();
constexpr uint64_t NANOSECONDS_PER_MILLISECOND = 1000000ULL;
constexpr uint32_t CAPTURE_AUDIO_START_TIMEOUT_MS = 1500;
constexpr auto CAPTURE_AUDIO_RETRY_INTERVAL = std::chrono::seconds(2);
constexpr const char* KMS_RGB_CAPS = "video/x-raw,format=BGRx";
constexpr size_t MAX_NETWORK_JPEG_SIZE = 8U * 1024U * 1024U;

// Keep the small legacy DRM request local so the node binary does not gain a
// libdrm link just to clear a plane left enabled by the deployed kmssink.
struct DrmModeSetPlaneRequest {
    uint32_t planeId;
    uint32_t crtcId;
    uint32_t fbId;
    uint32_t flags;
    int32_t crtcX;
    int32_t crtcY;
    uint32_t crtcW;
    uint32_t crtcH;
    uint32_t srcX;
    uint32_t srcY;
    uint32_t srcH;
    uint32_t srcW;
};

#define DMS_DRM_IOCTL_MODE_SETPLANE \
    _IOWR('d', 0xB7, DrmModeSetPlaneRequest)

bool disableDrmPlane(uint32_t planeId) {
    if (planeId == 0) return true;
    const int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (fd < 0) return false;
    DrmModeSetPlaneRequest request{};
    request.planeId = planeId;
    const bool disabled = ioctl(fd, DMS_DRM_IOCTL_MODE_SETPLANE, &request) == 0;
    close(fd);
    return disabled;
}

bool isSupportedCapturePixelFormat(const std::string& format) {
    return format == "auto" || format == "NV16" || format == "NV61" ||
           format == "NV12" || format == "NV21" || format == "YUY2" ||
           format == "YUYV" || format == "YVYU" || format == "UYVY" ||
           format == "VYUY";
}

bool isSupportedColorimetry(const std::string& colorimetry) {
    return colorimetry == "auto" || colorimetry == "bt601" ||
           colorimetry == "bt709" || colorimetry == "bt2020";
}

std::string gstCapturePixelFormat(const std::string& format) {
    return format == "YUYV" ? "YUY2" : format;
}

std::string captureCapsDescription(const CaptureOptions& options) {
    std::ostringstream caps;
    caps << "video/x-raw";
    if (options.pixelFormat != "auto") {
        caps << ",format=" << gstCapturePixelFormat(options.pixelFormat);
    }
    if (options.width != 0) {
        caps << ",width=" << options.width << ",height=" << options.height;
    }
    if (options.framerateNumerator != 0) {
        caps << ",framerate=" << options.framerateNumerator
             << '/' << options.framerateDenominator;
    }
    if (options.colorimetry != "auto") {
        caps << ",colorimetry=" << options.colorimetry;
    }
    return caps.str();
}

CaptureOptions captureOptionsForSource(const CaptureOptions& defaults,
                                       const SignalSourceConfig& source) {
    CaptureOptions capture = defaults;
    capture.devicePath = source.endpoint;
    if (source.width != 0 && source.height != 0) {
        capture.width = source.width;
        capture.height = source.height;
    }
    if (source.framerateNumerator != 0) {
        capture.framerateNumerator = source.framerateNumerator;
        capture.framerateDenominator = source.framerateDenominator;
    }
    // Automatic source fields inherit the format validated for this hardware.
    // Letting downstream caps choose makes RK628 retain dimensions and chroma
    // ordering requested by whichever process used /dev/video0 most recently.
    if (source.pixelFormat != "auto") capture.pixelFormat = source.pixelFormat;
    return capture;
}

bool ioctlRetry(int fd, unsigned long request, void* argument) {
    int result = 0;
    do {
        result = ioctl(fd, request, argument);
    } while (result < 0 && errno == EINTR);
    return result == 0;
}

uint32_t v4l2CapturePixelFormat(const std::string& format) {
    if (format == "auto") return 0;
    if (format == "NV16") return V4L2_PIX_FMT_NV16;
    if (format == "NV61") return V4L2_PIX_FMT_NV61;
    if (format == "NV12") return V4L2_PIX_FMT_NV12;
    if (format == "NV21") return V4L2_PIX_FMT_NV21;
    if (format == "YUY2" || format == "YUYV") return V4L2_PIX_FMT_YUYV;
    if (format == "YVYU") return V4L2_PIX_FMT_YVYU;
    if (format == "UYVY") return V4L2_PIX_FMT_UYVY;
    if (format == "VYUY") return V4L2_PIX_FMT_VYUY;
    return 0;
}

std::string v4l2PixelFormatName(uint32_t format) {
    std::string name(4, ' ');
    name[0] = static_cast<char>(format & 0xffU);
    name[1] = static_cast<char>((format >> 8U) & 0xffU);
    name[2] = static_cast<char>((format >> 16U) & 0xffU);
    name[3] = static_cast<char>((format >> 24U) & 0xffU);
    for (char& value : name) {
        if (value < 0x20 || value > 0x7e) value = '?';
    }
    return name;
}

bool configureCaptureFormat(int fd, uint32_t capabilities, const CaptureOptions& options,
                            uint32_t& width, uint32_t& height, uint32_t& pixelFormat) {
    v4l2_format format {};
    const bool multiplanar = (capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE) != 0U;
    if (multiplanar) {
        format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    } else if ((capabilities & V4L2_CAP_VIDEO_CAPTURE) != 0U) {
        format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    } else {
        errno = ENOTSUP;
        return false;
    }

    const uint32_t requestedPixelFormat = v4l2CapturePixelFormat(options.pixelFormat);
    const bool needsCurrentFormat = options.width == 0 || requestedPixelFormat == 0;
    if (needsCurrentFormat && !ioctlRetry(fd, VIDIOC_G_FMT, &format)) {
        return false;
    }

    uint32_t requestedWidth = options.width;
    uint32_t requestedHeight = options.height;
    uint32_t effectivePixelFormat = requestedPixelFormat;
    if (multiplanar) {
        if (requestedWidth == 0) {
            requestedWidth = format.fmt.pix_mp.width;
            requestedHeight = format.fmt.pix_mp.height;
        }
        if (effectivePixelFormat == 0) effectivePixelFormat = format.fmt.pix_mp.pixelformat;
        format.fmt.pix_mp.width = requestedWidth;
        format.fmt.pix_mp.height = requestedHeight;
        format.fmt.pix_mp.pixelformat = effectivePixelFormat;
        format.fmt.pix_mp.field = V4L2_FIELD_ANY;
    } else {
        if (requestedWidth == 0) {
            requestedWidth = format.fmt.pix.width;
            requestedHeight = format.fmt.pix.height;
        }
        if (effectivePixelFormat == 0) effectivePixelFormat = format.fmt.pix.pixelformat;
        format.fmt.pix.width = requestedWidth;
        format.fmt.pix.height = requestedHeight;
        format.fmt.pix.pixelformat = effectivePixelFormat;
        format.fmt.pix.field = V4L2_FIELD_ANY;
    }

    // A cold RKCIF node can report 0x0 until S_FMT is issued. This only
    // programs the capture memory format; HDMI lock is checked separately.
    if (requestedWidth != 0 && requestedHeight != 0 && effectivePixelFormat != 0 &&
        !ioctlRetry(fd, VIDIOC_S_FMT, &format)) {
        return false;
    }

    if (multiplanar) {
        width = format.fmt.pix_mp.width;
        height = format.fmt.pix_mp.height;
        pixelFormat = format.fmt.pix_mp.pixelformat;
    } else {
        width = format.fmt.pix.width;
        height = format.fmt.pix.height;
        pixelFormat = format.fmt.pix.pixelformat;
    }
    return true;
}

bool probeCaptureFrame(int fd, uint32_t capabilities, uint32_t timeoutMs,
                       std::string& error) {
    const bool multiplanar = (capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE) != 0U;
    const bool singlePlanar = (capabilities & V4L2_CAP_VIDEO_CAPTURE) != 0U;
    if (!multiplanar && !singlePlanar) {
        error = "Capture device has no streaming video capability";
        return false;
    }

    const v4l2_buf_type type = multiplanar
        ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
        : V4L2_BUF_TYPE_VIDEO_CAPTURE;
    v4l2_requestbuffers request {};
    request.count = 2;
    request.type = type;
    request.memory = V4L2_MEMORY_MMAP;
    if (!ioctlRetry(fd, VIDIOC_REQBUFS, &request) || request.count == 0) {
        error = "Capture device cannot allocate streaming buffers: " +
                std::string(std::strerror(errno));
        return false;
    }

    struct MappedBuffer {
        std::array<void*, VIDEO_MAX_PLANES> address{};
        std::array<size_t, VIDEO_MAX_PLANES> length{};
        uint32_t planeCount = 0;
    };
    std::vector<MappedBuffer> buffers(request.count);
    auto releaseBuffers = [&]() {
        for (MappedBuffer& buffer : buffers) {
            for (uint32_t plane = 0; plane < buffer.planeCount; ++plane) {
                if (buffer.address[plane] != MAP_FAILED && buffer.address[plane] != nullptr) {
                    munmap(buffer.address[plane], buffer.length[plane]);
                }
            }
            buffer = {};
        }
        v4l2_requestbuffers release {};
        release.count = 0;
        release.type = type;
        release.memory = V4L2_MEMORY_MMAP;
        ioctlRetry(fd, VIDIOC_REQBUFS, &release);
    };

    for (uint32_t index = 0; index < request.count; ++index) {
        v4l2_buffer buffer {};
        buffer.type = type;
        buffer.memory = V4L2_MEMORY_MMAP;
        std::array<v4l2_plane, VIDEO_MAX_PLANES> planes{};
        if (multiplanar) {
            buffer.length = VIDEO_MAX_PLANES;
            buffer.m.planes = planes.data();
        }
        buffer.index = index;
        if (!ioctlRetry(fd, VIDIOC_QUERYBUF, &buffer)) {
            error = "Capture device buffer query failed: " + std::string(std::strerror(errno));
            releaseBuffers();
            return false;
        }

        const uint32_t planeCount = multiplanar
            ? std::min(buffer.length, static_cast<uint32_t>(VIDEO_MAX_PLANES))
            : 1U;
        buffers[index].planeCount = planeCount;
        for (uint32_t plane = 0; plane < planeCount; ++plane) {
            const size_t length = multiplanar ? planes[plane].length : buffer.length;
            const off_t offset = multiplanar ? planes[plane].m.mem_offset : buffer.m.offset;
            void* address = mmap(nullptr, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, offset);
            if (address == MAP_FAILED) {
                error = "Capture device buffer mapping failed: " + std::string(std::strerror(errno));
                releaseBuffers();
                return false;
            }
            buffers[index].address[plane] = address;
            buffers[index].length[plane] = length;
        }
        if (!ioctlRetry(fd, VIDIOC_QBUF, &buffer)) {
            error = "Capture device buffer queue failed: " + std::string(std::strerror(errno));
            releaseBuffers();
            return false;
        }
    }

    int streamType = type;
    if (!ioctlRetry(fd, VIDIOC_STREAMON, &streamType)) {
        error = "Capture device stream start failed: " + std::string(std::strerror(errno));
        releaseBuffers();
        return false;
    }

    pollfd descriptor {};
    descriptor.fd = fd;
    descriptor.events = POLLIN | POLLPRI;
    const int waitResult = poll(&descriptor, 1, static_cast<int>(timeoutMs));
    bool frameReceived = false;
    if (waitResult > 0 && (descriptor.revents & (POLLIN | POLLPRI))) {
        v4l2_buffer buffer {};
        buffer.type = type;
        buffer.memory = V4L2_MEMORY_MMAP;
        std::array<v4l2_plane, VIDEO_MAX_PLANES> planes{};
        if (multiplanar) {
            buffer.length = VIDEO_MAX_PLANES;
            buffer.m.planes = planes.data();
        }
        if (ioctlRetry(fd, VIDIOC_DQBUF, &buffer)) {
            uint32_t bytesUsed = 0;
            if (multiplanar) {
                const uint32_t planeCount =
                    std::min(buffer.length, static_cast<uint32_t>(VIDEO_MAX_PLANES));
                for (uint32_t plane = 0; plane < planeCount; ++plane) {
                    bytesUsed += planes[plane].bytesused;
                }
            } else {
                bytesUsed = buffer.bytesused;
            }
            frameReceived = bytesUsed != 0;
        }
    }
    const int savedError = errno;
    ioctlRetry(fd, VIDIOC_STREAMOFF, &streamType);
    releaseBuffers();
    if (!frameReceived) {
        error = waitResult == 0
            ? "Capture device produced no frame before timeout"
            : "Capture device did not return a usable frame: " + std::string(std::strerror(savedError));
    }
    return frameReceived;
}

bool queryRk628SignalTimings(const std::string& captureDevice,
                             uint32_t& width, uint32_t& height,
                             std::string& error) {
    // The RKCIF video node forwards QUERY_DV_TIMINGS to the RK628 subdevice;
    // this is the same path used by the production v4l2-ctl diagnostics and
    // works even when subdevice character nodes are not exposed in /dev.
    const int videoFd = open(captureDevice.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (videoFd >= 0) {
        v4l2_dv_timings timings {};
        const bool queried = ioctlRetry(videoFd, VIDIOC_QUERY_DV_TIMINGS, &timings);
        const int savedError = errno;
        close(videoFd);
        if (queried && timings.type == V4L2_DV_BT_656_1120 &&
            timings.bt.width != 0 && timings.bt.height != 0) {
            width = timings.bt.width;
            height = timings.bt.height;
            error.clear();
            return true;
        }
        if (!queried) {
            error = "HDMI timing query failed on " + captureDevice + ": " +
                    std::strerror(savedError);
        } else {
            error = "HDMI timing query returned empty timings on " + captureDevice;
        }
    } else {
        error = "Cannot open capture device " + captureDevice + ": " +
                std::strerror(errno);
    }

    const std::filesystem::path videoClass("/sys/class/video4linux");
    std::error_code filesystemError;
    std::vector<std::filesystem::path> candidates;
    for (std::filesystem::directory_iterator iterator(
             videoClass, std::filesystem::directory_options::skip_permission_denied,
             filesystemError), end;
         !filesystemError && iterator != end; iterator.increment(filesystemError)) {
        const std::string deviceName = iterator->path().filename().string();
        if (deviceName.rfind("v4l-subdev", 0) != 0) continue;

        std::ifstream nameFile(iterator->path() / "name");
        std::string driverName;
        std::getline(nameFile, driverName);
        std::transform(driverName.begin(), driverName.end(), driverName.begin(),
                       [](unsigned char value) {
                           return static_cast<char>(std::tolower(value));
                       });
        if (driverName.find("rk628") != std::string::npos &&
            driverName.find("csi") != std::string::npos) {
            candidates.emplace_back(std::filesystem::path("/dev") / deviceName);
        }
    }

    if (filesystemError) {
        error = "Cannot enumerate V4L2 subdevices: " + filesystemError.message();
        return false;
    }
    if (candidates.empty()) {
        return false;
    }
    std::sort(candidates.begin(), candidates.end());

    std::string lastFailure;
    for (const std::filesystem::path& candidate : candidates) {
        const int fd = open(candidate.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            lastFailure = "Cannot open " + candidate.string() + ": " +
                          std::strerror(errno);
            continue;
        }

        v4l2_dv_timings timings {};
        const bool queried = ioctlRetry(fd, VIDIOC_QUERY_DV_TIMINGS, &timings);
        const int savedError = errno;
        close(fd);
        if (!queried) {
            lastFailure = "RK628 HDMI input is not locked on " + candidate.string() +
                          ": " + std::strerror(savedError);
            continue;
        }
        if (timings.type != V4L2_DV_BT_656_1120 ||
            timings.bt.width == 0 || timings.bt.height == 0) {
            lastFailure = "RK628 returned empty HDMI timings on " + candidate.string();
            continue;
        }

        width = timings.bt.width;
        height = timings.bt.height;
        error.clear();
        return true;
    }

    error = lastFailure.empty() ? "RK628 HDMI input is not locked" : lastFailure;
    return false;
}

std::string escapePipelineString(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size() + 8);
    for (const unsigned char character : value) {
        if (character < 0x20U || character == 0x7FU) {
            return {};
        }
        if (character == '\\' || character == '"') {
            escaped.push_back('\\');
        }
        escaped.push_back(static_cast<char>(character));
    }
    return escaped;
}

std::vector<AudioPlaybackOutput> selectedAudioOutputs(const CaptureOptions& options) {
    switch (options.audioOutputMode) {
        case AudioOutputMode::HDMI:
            return {options.hdmiAudioOutput};
        case AudioOutputMode::ANALOG:
            return {options.analogAudioOutput};
        case AudioOutputMode::BOTH:
            return {options.hdmiAudioOutput, options.analogAudioOutput};
    }
    return {};
}

bool isHttpEndpoint(const std::string& endpoint) {
    return endpoint.rfind("http://", 0) == 0 || endpoint.rfind("https://", 0) == 0;
}

struct NetworkInputContext {
    MediaPlayer* player = nullptr;
    std::vector<uint8_t>* pending = nullptr;
    void* appSrc = nullptr;
};

struct RtpEndpoint {
    std::string host;
    uint16_t port = 0;
};

bool parseRtpEndpoint(const std::string& endpoint, RtpEndpoint& result) {
    constexpr const char* prefix = "rtp://";
    if (endpoint.rfind(prefix, 0) != 0) return false;
    const size_t separator = endpoint.find(':', std::char_traits<char>::length(prefix));
    if (separator == std::string::npos || separator == std::char_traits<char>::length(prefix) ||
        separator + 1 >= endpoint.size()) {
        return false;
    }
    const std::string portText = endpoint.substr(separator + 1);
    if (portText.find_first_not_of("0123456789") != std::string::npos) return false;
    unsigned long port = 0;
    try {
        port = std::stoul(portText);
    } catch (...) {
        return false;
    }
    in_addr address{};
    const std::string host = endpoint.substr(std::char_traits<char>::length(prefix),
                                             separator - std::char_traits<char>::length(prefix));
    if (port == 0 || port > 65535 || inet_pton(AF_INET, host.c_str(), &address) != 1) {
        return false;
    }
    result.host = host;
    result.port = static_cast<uint16_t>(port);
    return true;
}

struct TransportEndpoint {
    std::string primary;
    std::string fallback;
};

TransportEndpoint splitTransportEndpoint(const std::string& endpoint) {
    constexpr const char* marker = "|fallback=";
    const size_t separator = endpoint.find(marker);
    if (separator == std::string::npos) return {endpoint, {}};
    return {endpoint.substr(0, separator),
            endpoint.substr(separator + std::char_traits<char>::length(marker))};
}

} // namespace

class MediaPlayer::Runtime {
public:
    using GstInitCheck = int (*)(int*, char***, GError**);
    using GstElementFactoryFind = void* (*)(const char*);
    using GstBinGetByName = void* (*)(void*, const char*);
    using GstParseLaunch = void* (*)(const char*, GError**);
    using GstElementSetState = int (*)(void*, int);
    using GstElementGetState = int (*)(void*, int*, int*, uint64_t);
    using GstElementSetLockedState = void (*)(void*, int);
    using GstElementSyncStateWithParent = int (*)(void*);
    using GstElementGetBus = void* (*)(void*);
    using GstBusTimedPopFiltered = void* (*)(void*, uint64_t, uint32_t);
    using GstMessageParseError = void (*)(void*, GError**, char**);
    using GstMiniObjectUnref = void (*)(void*);
    using GstObjectUnref = void (*)(void*);
    using GObjectSet = void (*)(void*, const char*, ...);
    using GErrorFree = void (*)(GError*);
    using GFree = void (*)(void*);
    using GstAppSinkTryPullSample = void* (*)(void*, uint64_t);
    using GstSampleGetBuffer = void* (*)(void*);
    using GstBufferGetSize = size_t (*)(void*);
    using GstBufferExtract = size_t (*)(void*, size_t, void*, size_t);
    using GstBufferNewAllocate = void* (*)(void*, size_t, void*);
    using GstBufferFill = size_t (*)(void*, size_t, const void*, size_t);
    using GstAppSrcPushBuffer = int (*)(void*, void*);
    using GstAppSrcEndOfStream = int (*)(void*);

    ~Runtime() {
        if (m_gstreamer != nullptr) {
            dlclose(m_gstreamer);
        }
        if (m_gobject != nullptr) {
            dlclose(m_gobject);
        }
        if (m_glib != nullptr) {
            dlclose(m_glib);
        }
        if (m_gstapp != nullptr) {
            dlclose(m_gstapp);
        }
    }

    bool initialize(std::string& error) {
        if (m_gstreamer != nullptr) {
            return true;
        }

        m_gstreamer = dlopen("libgstreamer-1.0.so.0", RTLD_NOW | RTLD_LOCAL);
        if (m_gstreamer == nullptr) {
            error = std::string("Cannot load GStreamer runtime: ") + dlerror();
            return false;
        }
        m_glib = dlopen("libglib-2.0.so.0", RTLD_NOW | RTLD_LOCAL);
        if (m_glib == nullptr) {
            error = std::string("Cannot load GLib runtime: ") + dlerror();
            return false;
        }

        if (!loadGstSymbol(gstInitCheck, "gst_init_check", error) ||
            !loadGstSymbol(gstElementFactoryFind, "gst_element_factory_find", error) ||
            !loadGstSymbol(gstBinGetByName, "gst_bin_get_by_name", error) ||
            !loadGstSymbol(gstParseLaunch, "gst_parse_launch", error) ||
            !loadGstSymbol(gstElementSetState, "gst_element_set_state", error) ||
            !loadGstSymbol(gstElementGetState, "gst_element_get_state", error) ||
            !loadGstSymbol(gstElementSetLockedState, "gst_element_set_locked_state", error) ||
            !loadGstSymbol(gstElementSyncStateWithParent,
                           "gst_element_sync_state_with_parent", error) ||
            !loadGstSymbol(gstElementGetBus, "gst_element_get_bus", error) ||
            !loadGstSymbol(gstBusTimedPopFiltered, "gst_bus_timed_pop_filtered", error) ||
            !loadGstSymbol(gstMessageParseError, "gst_message_parse_error", error) ||
            !loadGstSymbol(gstMiniObjectUnref, "gst_mini_object_unref", error) ||
            !loadGstSymbol(gstObjectUnref, "gst_object_unref", error) ||
            !loadGlibSymbol(gErrorFree, "g_error_free", error) ||
            !loadGlibSymbol(gFree, "g_free", error) ||
            !loadGObjectSymbol(gObjectSet, "g_object_set", error)) {
            return false;
        }

        int argc = 0;
        char** argv = nullptr;
        GError* initError = nullptr;
        if (gstInitCheck(&argc, &argv, &initError) == 0) {
            error = consumeError(initError, "GStreamer initialization failed");
            return false;
        }

        // The display path remains usable without the optional preview branch.
        // Production images enable gstapp/appsink and jpegenc for live previews.
        m_gstapp = dlopen("libgstapp-1.0.so.0", RTLD_NOW | RTLD_LOCAL);
        if (m_gstapp != nullptr) {
            std::string ignoredError;
            if (!loadSymbol(m_gstapp, gstAppSinkTryPullSample,
                            "gst_app_sink_try_pull_sample", ignoredError) ||
                !loadSymbol(m_gstapp, gstAppSrcPushBuffer,
                            "gst_app_src_push_buffer", ignoredError) ||
                !loadSymbol(m_gstapp, gstAppSrcEndOfStream,
                            "gst_app_src_end_of_stream", ignoredError) ||
                !loadGstSymbol(gstSampleGetBuffer, "gst_sample_get_buffer", ignoredError) ||
                !loadGstSymbol(gstBufferGetSize, "gst_buffer_get_size", ignoredError) ||
                !loadGstSymbol(gstBufferExtract, "gst_buffer_extract", ignoredError) ||
                !loadGstSymbol(gstBufferNewAllocate, "gst_buffer_new_allocate", ignoredError) ||
                !loadGstSymbol(gstBufferFill, "gst_buffer_fill", ignoredError)) {
                dlclose(m_gstapp);
                m_gstapp = nullptr;
                gstAppSinkTryPullSample = nullptr;
                gstAppSrcPushBuffer = nullptr;
                gstAppSrcEndOfStream = nullptr;
                gstSampleGetBuffer = nullptr;
                gstBufferGetSize = nullptr;
                gstBufferExtract = nullptr;
                gstBufferNewAllocate = nullptr;
                gstBufferFill = nullptr;
            }
        }

        constexpr std::array<const char*, 26> requiredFactories = {
            "filesrc", "parsebin", "decodebin", "mppvideodec", "mppjpegdec", "pngdec",
            "imagefreeze", "v4l2src", "watchdog", "videocrop", "videoconvert",
            "capsfilter", "textoverlay", "kmssink", "alsasrc", "alsasink",
            "audioconvert", "audioresample", "queue", "tee", "volume",
            "uridecodebin", "videoscale", "fakesink", "valve", "appsrc"
        };
        for (const char* factoryName : requiredFactories) {
            void* factory = gstElementFactoryFind(factoryName);
            if (factory == nullptr) {
                error = std::string("Required GStreamer plugin is unavailable: ") + factoryName;
                return false;
            }
            gstObjectUnref(factory);
        }
        return true;
    }

    bool previewAvailable() const {
        if (m_gstapp == nullptr || gstAppSinkTryPullSample == nullptr ||
            gstSampleGetBuffer == nullptr ||
            gstBufferGetSize == nullptr ||
            gstBufferExtract == nullptr) {
            return false;
        }
        void* jpegFactory = gstElementFactoryFind("jpegenc");
        void* appsinkFactory = gstElementFactoryFind("appsink");
        void* videoscaleFactory = gstElementFactoryFind("videoscale");
        const bool available = jpegFactory != nullptr && appsinkFactory != nullptr &&
                               videoscaleFactory != nullptr;
        if (jpegFactory != nullptr) gstObjectUnref(jpegFactory);
        if (appsinkFactory != nullptr) gstObjectUnref(appsinkFactory);
        if (videoscaleFactory != nullptr) gstObjectUnref(videoscaleFactory);
        return available;
    }

    bool networkInputAvailable() const {
        if (m_gstapp == nullptr || gstAppSrcPushBuffer == nullptr ||
            gstAppSrcEndOfStream == nullptr || gstBufferNewAllocate == nullptr ||
            gstBufferFill == nullptr) {
            return false;
        }
        constexpr std::array<const char*, 3> factories = {"appsrc", "mppjpegdec", "queue"};
        for (const char* factoryName : factories) {
            void* factory = gstElementFactoryFind(factoryName);
            if (factory == nullptr) return false;
            gstObjectUnref(factory);
        }
        return true;
    }

    bool h264RtpEncodeAvailable() const {
        constexpr std::array<const char*, 6> factories = {
            "mpph264enc", "h264parse", "rtph264pay", "udpsink", "queue", "videoconvert"};
        for (const char* factoryName : factories) {
            if (!hasFactory(factoryName)) return false;
        }
        return true;
    }

    bool h264RtpDecodeAvailable() const {
        constexpr std::array<const char*, 5> factories = {
            "udpsrc", "rtpjitterbuffer", "rtph264depay", "h264parse",
            "mppvideodec"};
        for (const char* factoryName : factories) {
            if (!hasFactory(factoryName)) return false;
        }
        return true;
    }

    bool hasFactory(const char* factoryName) const {
        if (factoryName == nullptr || gstElementFactoryFind == nullptr ||
            gstObjectUnref == nullptr) {
            return false;
        }
        void* factory = gstElementFactoryFind(factoryName);
        if (factory == nullptr) return false;
        gstObjectUnref(factory);
        return true;
    }

    bool hardwareJpegDecodeAvailable() const {
        return hasFactory("mppjpegdec");
    }

    std::string consumeError(GError* error, const std::string& fallback) const {
        if (error == nullptr) {
            return fallback;
        }
        const std::string message = error->message != nullptr ? error->message : fallback;
        gErrorFree(error);
        return message;
    }

    GstInitCheck gstInitCheck = nullptr;
    GstElementFactoryFind gstElementFactoryFind = nullptr;
    GstBinGetByName gstBinGetByName = nullptr;
    GstParseLaunch gstParseLaunch = nullptr;
    GstElementSetState gstElementSetState = nullptr;
    GstElementGetState gstElementGetState = nullptr;
    GstElementSetLockedState gstElementSetLockedState = nullptr;
    GstElementSyncStateWithParent gstElementSyncStateWithParent = nullptr;
    GstElementGetBus gstElementGetBus = nullptr;
    GstBusTimedPopFiltered gstBusTimedPopFiltered = nullptr;
    GstMessageParseError gstMessageParseError = nullptr;
    GstMiniObjectUnref gstMiniObjectUnref = nullptr;
    GstObjectUnref gstObjectUnref = nullptr;
    GObjectSet gObjectSet = nullptr;
    GErrorFree gErrorFree = nullptr;
    GFree gFree = nullptr;
    GstAppSinkTryPullSample gstAppSinkTryPullSample = nullptr;
    GstSampleGetBuffer gstSampleGetBuffer = nullptr;
    GstBufferGetSize gstBufferGetSize = nullptr;
    GstBufferExtract gstBufferExtract = nullptr;
    GstBufferNewAllocate gstBufferNewAllocate = nullptr;
    GstBufferFill gstBufferFill = nullptr;
    GstAppSrcPushBuffer gstAppSrcPushBuffer = nullptr;
    GstAppSrcEndOfStream gstAppSrcEndOfStream = nullptr;

private:
    template <typename Function>
    bool loadSymbol(void* library, Function& function, const char* name, std::string& error) {
        dlerror();
        function = reinterpret_cast<Function>(dlsym(library, name));
        const char* symbolError = dlerror();
        if (symbolError != nullptr || function == nullptr) {
            error = std::string("Missing runtime symbol ") + name + ": " +
                    (symbolError != nullptr ? symbolError : "unknown error");
            return false;
        }
        return true;
    }

    template <typename Function>
    bool loadGstSymbol(Function& function, const char* name, std::string& error) {
        return loadSymbol(m_gstreamer, function, name, error);
    }

    template <typename Function>
    bool loadGlibSymbol(Function& function, const char* name, std::string& error) {
        return loadSymbol(m_glib, function, name, error);
    }

    template <typename Function>
    bool loadGObjectSymbol(Function& function, const char* name, std::string& error) {
        if (m_gobject == nullptr) {
            m_gobject = dlopen("libgobject-2.0.so.0", RTLD_NOW | RTLD_LOCAL);
            if (m_gobject == nullptr) {
                error = std::string("Cannot load GObject runtime: ") + dlerror();
                return false;
            }
        }
        return loadSymbol(m_gobject, function, name, error);
    }

    void* m_gstreamer = nullptr;
    void* m_glib = nullptr;
    void* m_gobject = nullptr;
    void* m_gstapp = nullptr;
};

MediaPlayer::MediaPlayer()
    : m_nodeId(0)
    , m_runtime(std::make_unique<Runtime>())
    , m_pipeline(nullptr)
    , m_startupPipeline(nullptr)
    , m_backgroundPipeline(nullptr)
    , m_captureAudioPipeline(nullptr)
    , m_captureAudioVolumePercent(100)
    , m_nextCaptureAudioRetry(std::chrono::steady_clock::time_point::min())
    , m_state(State::Idle)
    , m_initialized(false)
    , m_rtpEncodeDisabled(false)
    , m_previewSink(nullptr)
    , m_previewRunning(false)
    , m_networkAppSrc(nullptr)
    , m_networkRunning(false)
    , m_signalFrameWidth(0)
    , m_signalFrameHeight(0)
    , m_signalOutputActive(false) {
}

MediaPlayer::~MediaPlayer() {
    std::lock_guard<std::mutex> lock(m_mutex);
    releasePipelineUnlocked();
    releaseBackgroundPipelineUnlocked();
}

bool MediaPlayer::initialize(uint32_t nodeId, const MediaPlayerOptions& options) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_pipeline != nullptr || m_backgroundPipeline != nullptr) {
        setErrorUnlocked("Cannot reinitialize while media is prepared");
        return false;
    }
    if (nodeId == 0) {
        setErrorUnlocked("Node ID must not be zero");
        return false;
    }
    if (options.startupTimeoutMs == 0) {
        setErrorUnlocked("Startup timeout must be greater than zero");
        return false;
    }
    if (options.connectorId > static_cast<uint32_t>(std::numeric_limits<int>::max()) ||
        options.planeId > static_cast<uint32_t>(std::numeric_limits<int>::max()) ||
        options.backgroundPlaneId > static_cast<uint32_t>(std::numeric_limits<int>::max())) {
        setErrorUnlocked("DRM connector/plane ID is out of range");
        return false;
    }
    std::set<uint32_t> overlayPlanes;
    for (const uint32_t planeId : options.overlayPlaneIds) {
        if (planeId == 0 || planeId > static_cast<uint32_t>(std::numeric_limits<int>::max()) ||
            !overlayPlanes.insert(planeId).second) {
            setErrorUnlocked("DRM overlay plane IDs must be non-zero and unique");
            return false;
        }
    }
    if ((options.outputWidth == 0) != (options.outputHeight == 0) ||
        options.outputWidth > 16384 || options.outputHeight > 16384) {
        setErrorUnlocked("DRM output dimensions are invalid");
        return false;
    }
    if (options.capture.devicePath.empty()) {
        setErrorUnlocked("Capture device path must not be empty");
        return false;
    }
    if (!isSupportedCapturePixelFormat(options.capture.pixelFormat)) {
        setErrorUnlocked("Unsupported capture pixel format: " + options.capture.pixelFormat);
        return false;
    }
    if ((options.capture.width == 0) != (options.capture.height == 0)) {
        setErrorUnlocked("Capture width and height must both be automatic or explicit");
        return false;
    }
    if (options.capture.width > 16384 || options.capture.height > 16384) {
        setErrorUnlocked("Capture dimensions are out of range");
        return false;
    }
    if (options.capture.framerateDenominator == 0 ||
        options.capture.framerateNumerator > 1000 ||
        options.capture.framerateDenominator > 1000) {
        setErrorUnlocked("Capture frame rate is out of range");
        return false;
    }
    if (!isSupportedColorimetry(options.capture.colorimetry)) {
        setErrorUnlocked("Unsupported capture colorimetry: " + options.capture.colorimetry);
        return false;
    }
    if (options.capture.audioEnabled && options.capture.audioCaptureDevice.empty()) {
        setErrorUnlocked("Audio capture device must not be empty when capture audio is enabled");
        return false;
    }
    if (options.capture.audioEnabled) {
        for (const AudioPlaybackOutput* output : {
                 &options.capture.hdmiAudioOutput, &options.capture.analogAudioOutput}) {
            if (output->device.empty() || !std::isfinite(output->volume) ||
                output->volume <= 0.0 || output->volume > 64.0) {
                setErrorUnlocked("Audio playback output device or volume is invalid");
                return false;
            }
        }
    }

    std::string runtimeError;
    if (!m_runtime->initialize(runtimeError)) {
        setErrorUnlocked(runtimeError);
        return false;
    }

    m_nodeId = nodeId;
    m_options = options;
    m_state = State::Idle;
    m_initialized = true;
    m_lastError.clear();
    LOG_INFO("GStreamer RKMPP/KMS player initialized: connector=%u, videoPlane=%u, "
             "backgroundPlane=%u fullscreen=%s output=%ux%u",
             m_options.connectorId, m_options.planeId, m_options.backgroundPlaneId,
             m_options.fullscreen ? "true" : "false",
             m_options.outputWidth, m_options.outputHeight);
    return true;
}

bool MediaPlayer::showImage(const std::string& imagePath, const std::string& nodeIp) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_initialized) {
        setErrorUnlocked("Media player is not initialized");
        return false;
    }
    if (nodeIp.empty()) {
        setErrorUnlocked("Node IP is required for the idle image");
        return false;
    }
    if (imagePath.empty() || !std::filesystem::is_regular_file(imagePath)) {
        setErrorUnlocked("Idle image file does not exist: " + imagePath);
        return false;
    }

    const std::string absolutePath = std::filesystem::absolute(imagePath).lexically_normal().string();
    const std::string escapedPath = escapePipelineString(absolutePath);
    if (escapedPath.empty()) {
        setErrorUnlocked("Idle image path contains unsupported control characters");
        return false;
    }
    const std::string overlayText = "NODE IP: " + nodeIp;
    const std::string escapedOverlayText = escapePipelineString(overlayText);
    if (escapedOverlayText.empty()) {
        setErrorUnlocked("Node IP contains unsupported control characters");
        return false;
    }

    const bool separateBackground = m_options.backgroundPlaneId != 0 &&
                                    m_options.backgroundPlaneId != m_options.planeId;
    if (separateBackground && m_backgroundPipeline != nullptr &&
        pollPipelineBusUnlocked(m_backgroundPipeline, true)) {
        if (m_pipeline == nullptr) m_state = State::Image;
        m_lastError.clear();
        return true;
    }
    if (separateBackground) {
        releaseBackgroundPipelineUnlocked();
    } else {
        releasePipelineUnlocked(true);
    }

    std::ostringstream pipelineDescription;
    pipelineDescription
        << "filesrc location=\"" << escapedPath << "\" ! "
        << "decodebin ! "
        << "imagefreeze is-live=true ! "
        << "videoconvert name=dms_idle_convert ! videoscale ! "
        << kmsVideoCaps() << " ! "
        << "textoverlay name=dms_idle_ip text=\"" << escapedOverlayText
        << "\" valignment=bottom halignment=center xpad=0 ypad=40 "
        << "font-desc=\"Sans 10\" color=0xd9ffffff outline-color=0x90000000 "
        << "draw-shadow=false shaded-background=false ! "
        << kmsSinkDescription(false, true,
                              separateBackground ? m_options.backgroundPlaneId
                              : m_options.planeId,
                              false);

    GError* parseError = nullptr;
    void*& imagePipeline = separateBackground ? m_backgroundPipeline : m_pipeline;
    imagePipeline = m_runtime->gstParseLaunch(pipelineDescription.str().c_str(), &parseError);
    if (imagePipeline == nullptr || parseError != nullptr) {
        const std::string error = m_runtime->consumeError(parseError, "Cannot create idle image pipeline");
        if (imagePipeline != nullptr) {
            m_runtime->gstObjectUnref(imagePipeline);
            imagePipeline = nullptr;
        }
        setErrorUnlocked("Cannot create idle image pipeline: " + error);
        return false;
    }

    m_lastError.clear();
    if (!transitionPipelineUnlocked(imagePipeline, GST_STATE_PLAYING,
                                    m_options.startupTimeoutMs,
                                    "display idle image", separateBackground)) {
        if (separateBackground) {
            releaseBackgroundPipelineUnlocked();
        } else {
            releasePipelineUnlocked();
        }
        return false;
    }

    // The new sink has presented its first frame on the same plane. Only now
    // release the EOS animation's final buffer, so no empty scanout is exposed.
    releaseStartupPipelineUnlocked();
    // A separate background plane can be updated while the capture pipeline
    // remains alive for the client preview. Do not hide that capture state or
    // the scheduler will probe/reopen the busy V4L2 device repeatedly.
    if (!separateBackground || m_pipeline == nullptr) m_state = State::Image;
    LOG_INFO("Idle image displayed on %s plane %u: path=%s, nodeIp=%s",
             separateBackground ? "background" : "video",
             separateBackground ? m_options.backgroundPlaneId : m_options.planeId,
             absolutePath.c_str(), nodeIp.c_str());
    return true;
}


bool MediaPlayer::captureAvailable(const SignalSourceConfig& source) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_initialized) {
        m_lastError = "Media player is not initialized";
        return false;
    }

    if (source.type != SignalSourceType::CAPTURE) {
        m_signalFrameWidth = source.width;
        m_signalFrameHeight = source.height;
        m_lastError.clear();
        return source.type != SignalSourceType::NONE;
    }

    const CaptureOptions capture = captureOptionsForSource(m_options.capture, source);
    uint32_t signalWidth = 0;
    uint32_t signalHeight = 0;
    std::string signalError;
    if (!queryRk628SignalTimings(capture.devicePath, signalWidth, signalHeight,
                                 signalError)) {
        m_lastError = signalError;
        return false;
    }

    const int fd = open(capture.devicePath.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        m_lastError = "Cannot open capture device " + capture.devicePath + ": " +
                      std::strerror(errno);
        return false;
    }

    v4l2_capability capability {};
    if (!ioctlRetry(fd, VIDIOC_QUERYCAP, &capability)) {
        const int savedError = errno;
        close(fd);
        m_lastError = "Cannot query capture device " + capture.devicePath + ": " +
                      std::strerror(savedError);
        return false;
    }

    const uint32_t capabilities =
        (capability.capabilities & V4L2_CAP_DEVICE_CAPS) != 0U
            ? capability.device_caps
            : capability.capabilities;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t pixelFormat = 0;
    if (!configureCaptureFormat(fd, capabilities, capture, width, height, pixelFormat)) {
        const int savedError = errno;
        close(fd);
        m_lastError = "Cannot configure capture format on " + capture.devicePath + ": " +
                      std::strerror(savedError);
        return false;
    }

    std::string frameError;
    if (!probeCaptureFrame(fd, capabilities, 500, frameError)) {
        close(fd);
        m_lastError = frameError;
        return false;
    }

    close(fd);

    // QUERY_DV_TIMINGS plus a real V4L2 frame is the authoritative RK628 lock
    // check. S_FMT alone can succeed while HDMI is disconnected.
    if (width == 0 || height == 0 || pixelFormat == 0) {
        m_lastError = "HDMI capture signal is not locked (active format is 0x0)";
        return false;
    }
    if (capture.width != 0 && (width != capture.width || height != capture.height)) {
        std::ostringstream error;
        error << "HDMI capture format " << width << 'x' << height
              << " does not match configured " << capture.width << 'x' << capture.height;
        m_lastError = error.str();
        return false;
    }
    const uint32_t configuredPixelFormat = v4l2CapturePixelFormat(capture.pixelFormat);
    if (configuredPixelFormat != 0 && pixelFormat != configuredPixelFormat) {
        m_lastError = "Capture device accepted pixel format " + v4l2PixelFormatName(pixelFormat) +
                      " instead of configured " + capture.pixelFormat;
        return false;
    }

    LOG_INFO("HDMI capture locked: source=%ux%u capture=%ux%u format=%s",
             signalWidth, signalHeight, width, height,
             v4l2PixelFormatName(pixelFormat).c_str());
    m_signalFrameWidth = width;
    m_signalFrameHeight = height;
    m_lastError.clear();
    return true;
}

bool MediaPlayer::setNodeId(uint32_t nodeId) {
    if (nodeId == 0) return false;
    std::lock_guard<std::mutex> lock(m_mutex);
    m_nodeId = nodeId;
    return true;
}

bool MediaPlayer::showCapture() {
    SignalSourceConfig source;
    source.type = SignalSourceType::CAPTURE;
    source.endpoint = m_options.capture.devicePath;
    source.width = m_options.capture.width;
    source.height = m_options.capture.height;
    source.framerateNumerator = m_options.capture.framerateNumerator;
    source.framerateDenominator = m_options.capture.framerateDenominator;
    source.pixelFormat = m_options.capture.pixelFormat;
    return showSignal(source);
}

bool MediaPlayer::showSignal(const SignalSourceConfig& source) {
    std::lock_guard<std::mutex> lock(m_mutex);
    return showSignalUnlocked(source, true);
}

Protocol::SetWindowLayoutCommand MediaPlayer::prepareWindowRenderCommandUnlocked(
    const Protocol::SetWindowLayoutCommand& command) const {
    Protocol::SetWindowLayoutCommand renderCommand = command;
    if (m_options.outputWidth == 0 || m_options.outputHeight == 0 ||
        (m_options.outputWidth == command.outputWidth &&
         m_options.outputHeight == command.outputHeight)) {
        return renderCommand;
    }

    const auto scaleEdge = [](uint32_t value, uint32_t from,
                              uint32_t to) -> uint32_t {
        return static_cast<uint32_t>(std::llround(
            static_cast<double>(value) * static_cast<double>(to) /
            static_cast<double>(from)));
    };
    renderCommand.outputWidth = m_options.outputWidth;
    renderCommand.outputHeight = m_options.outputHeight;
    for (auto& layer : renderCommand.layers) {
        const uint32_t right = layer.targetX + layer.targetWidth;
        const uint32_t bottom = layer.targetY + layer.targetHeight;
        layer.targetX = scaleEdge(layer.targetX, command.outputWidth,
                                  renderCommand.outputWidth);
        layer.targetY = scaleEdge(layer.targetY, command.outputHeight,
                                  renderCommand.outputHeight);
        const uint32_t targetRight = scaleEdge(right, command.outputWidth,
                                               renderCommand.outputWidth);
        const uint32_t targetBottom = scaleEdge(bottom, command.outputHeight,
                                                renderCommand.outputHeight);
        layer.targetWidth = targetRight > layer.targetX
            ? targetRight - layer.targetX : 1U;
        layer.targetHeight = targetBottom > layer.targetY
            ? targetBottom - layer.targetY : 1U;
    }
    LOG_INFO("Window layout scaled for local DRM output: command=%ux%u local=%ux%u",
             command.outputWidth, command.outputHeight,
             renderCommand.outputWidth, renderCommand.outputHeight);
    return renderCommand;
}

bool MediaPlayer::showWindowLayoutHardwareUnlocked(
    const Protocol::SetWindowLayoutCommand& command) {
    std::vector<const Protocol::WindowLayer*> orderedLayers;
    orderedLayers.reserve(command.layers.size());
    for (const auto& layer : command.layers) orderedLayers.push_back(&layer);
    std::stable_sort(orderedLayers.begin(), orderedLayers.end(),
        [](const Protocol::WindowLayer* left, const Protocol::WindowLayer* right) {
            return left->zOrder < right->zOrder;
        });

    const bool fullscreen = orderedLayers.size() == 1 && command.sources.size() == 1 &&
        orderedLayers.front()->sourceX == 0 && orderedLayers.front()->sourceY == 0 &&
        orderedLayers.front()->sourceWidth == command.sources.front().width &&
        orderedLayers.front()->sourceHeight == command.sources.front().height &&
        orderedLayers.front()->targetX == 0 && orderedLayers.front()->targetY == 0 &&
        orderedLayers.front()->targetWidth == command.outputWidth &&
        orderedLayers.front()->targetHeight == command.outputHeight;

    std::vector<uint32_t> planeIds;
    if (fullscreen) {
        // Live video must take the first verified hardware window.  On the
        // RK3566 VOP2 all Smart0 windows report immutable normalized-zpos=0;
        // Smart0-win0 therefore wins the hardware overlap even when kmssink
        // accepts a requested zpos for another window.  Using the first
        // overlay plane also makes fullscreen and single-window layouts obey
        // the same physical plane ordering instead of leaving fbcon above the
        // decoded framebuffer.
        if (!m_options.overlayPlaneIds.empty()) {
            planeIds.push_back(m_options.overlayPlaneIds.front());
        } else {
            planeIds.push_back(m_options.planeId != 0
                                   ? m_options.planeId : m_options.backgroundPlaneId);
        }
    } else {
        if (m_options.overlayPlaneIds.size() < orderedLayers.size()) {
            std::ostringstream error;
            error << "Hardware window layout requires " << orderedLayers.size()
                  << " DRM overlay planes; configured " << m_options.overlayPlaneIds.size();
            setErrorUnlocked(error.str());
            return false;
        }
        planeIds.assign(m_options.overlayPlaneIds.begin(),
                        m_options.overlayPlaneIds.begin() + orderedLayers.size());
    }
    for (const uint32_t planeId : planeIds) {
        if (planeId == 0 && !fullscreen) {
            setErrorUnlocked("Hardware window layout contains an unassigned DRM plane");
            return false;
        }
    }

    for (const auto* layer : orderedLayers) {
        if (layer->targetWidth == 0 || layer->targetHeight == 0 ||
            layer->targetX + layer->targetWidth > command.outputWidth ||
            layer->targetY + layer->targetHeight > command.outputHeight) {
            setErrorUnlocked("Hardware window layer is outside the DRM output");
            return false;
        }
        const auto source = std::find_if(command.sources.begin(), command.sources.end(),
            [layer](const Protocol::WindowSource& value) {
                return value.sourceNodeId == layer->sourceNodeId;
            });
        if (source == command.sources.end() || source->sourceNodeId == 0 ||
            source->width == 0 || source->height == 0 ||
            source->width > 16384 || source->height > 16384 ||
            layer->sourceX != 0 || layer->sourceY != 0 ||
            layer->sourceWidth != source->width || layer->sourceHeight != source->height) {
            setErrorUnlocked("Hardware DRM planes require a full-frame source crop");
            return false;
        }
    }

    std::ostringstream pipelineDescription;
    std::vector<std::pair<void*, std::string>> networkSources;
    std::vector<uint32_t> networkSourceIds;
    networkSources.reserve(command.sources.size());
    networkSourceIds.reserve(command.sources.size());
    for (const auto& source : command.sources) {
        pipelineDescription << ' ';
        const TransportEndpoint transport = splitTransportEndpoint(source.endpoint);
        RtpEndpoint rtpEndpoint;
        const bool useRtp = parseRtpEndpoint(transport.primary, rtpEndpoint) &&
                            m_runtime->h264RtpDecodeAvailable();
        const bool useHttp = !useRtp && isHttpEndpoint(
            transport.fallback.empty() ? transport.primary : transport.fallback) &&
            m_runtime->networkInputAvailable() && m_runtime->hardwareJpegDecodeAvailable();
        if (!useRtp && !useHttp) {
            setErrorUnlocked("Window source requires RTP/H.264 or hardware JPEG input: " +
                             std::to_string(source.sourceNodeId));
            return false;
        }
        const std::string teeName = "dms_window_src_" + std::to_string(source.sourceNodeId);
        if (useRtp) {
            pipelineDescription
                << "udpsrc multicast-group=" << rtpEndpoint.host
                << " port=" << rtpEndpoint.port
                << " auto-multicast=true buffer-size=4194304 reuse=true "
                << "caps=\"application/x-rtp,media=video,encoding-name=H264,clock-rate=90000,payload=96\" ! "
                   "rtpjitterbuffer latency=80 drop-on-latency=true do-lost=true ! "
                   "rtph264depay ! h264parse config-interval=1 ! "
                   "mppvideodec fast-mode=true dma-feature=true ! "
                   "video/x-raw,format=BGRx ! tee name=" << teeName;
        } else {
            const std::string appSrcName = "dms_window_input_" +
                std::to_string(source.sourceNodeId);
            pipelineDescription
                << "appsrc name=" << appSrcName
                << " is-live=false format=bytes do-timestamp=false block=true "
                   "leaky-type=downstream max-buffers=2 max-bytes=8388608 "
                   "caps=\"image/jpeg\" ! "
                   "queue leaky=downstream max-size-buffers=2 ! "
                   "jpegdec ! videoconvert ! "
                "video/x-raw,format=BGRx ! tee name="
                << teeName;
            networkSources.emplace_back(nullptr,
                transport.fallback.empty() ? transport.primary : transport.fallback);
            networkSourceIds.push_back(source.sourceNodeId);
        }
        for (const auto* layer : orderedLayers) {
            if (layer->sourceNodeId != source.sourceNodeId) continue;
            const size_t layerIndex = static_cast<size_t>(
                std::distance(orderedLayers.begin(),
                              std::find(orderedLayers.begin(), orderedLayers.end(), layer)));
            pipelineDescription << " " << teeName << ". ! queue leaky=downstream "
                << "max-size-buffers=2 max-size-bytes=0 max-size-time=0 ! "
                << kmsWindowSinkDescription(*layer, planeIds[layerIndex], layerIndex);
        }
    }

    GError* parseError = nullptr;
    void* pipeline = m_runtime->gstParseLaunch(pipelineDescription.str().c_str(), &parseError);
    if (pipeline == nullptr || parseError != nullptr) {
        const std::string error = m_runtime->consumeError(
            parseError, "Cannot create hardware window pipeline");
        if (pipeline != nullptr) m_runtime->gstObjectUnref(pipeline);
        setErrorUnlocked("Cannot create hardware window pipeline: " + error);
        return false;
    }

    for (size_t index = 0; index < networkSources.size(); ++index) {
        auto& networkSource = networkSources[index];
        const std::string appSrcName = "dms_window_input_" +
            std::to_string(networkSourceIds[index]);
        networkSource.first = m_runtime->gstBinGetByName(pipeline, appSrcName.c_str());
        if (networkSource.first == nullptr) {
            for (auto& acquiredSource : networkSources) {
                if (acquiredSource.first != nullptr) {
                    m_runtime->gstObjectUnref(acquiredSource.first);
                    acquiredSource.first = nullptr;
                }
            }
            m_runtime->gstObjectUnref(pipeline);
            setErrorUnlocked("Hardware window pipeline is missing its network input appsrc");
            return false;
        }
    }

    // Parse the complete hardware pipeline before releasing the currently
    // displayed frame.  The first live layer intentionally reuses the idle
    // plane on RK3566, so the idle owner must be released before PLAYING; the
    // previous live pipeline is then released before its planes are reused.
    releaseBackgroundPipelineUnlocked();
    releasePipelineUnlocked();
    m_pipeline = pipeline;
    m_signalFrameWidth = command.outputWidth;
    m_signalFrameHeight = command.outputHeight;
    m_signalSource = {};
    m_signalOutputActive = true;

    m_networkRunning.store(true);
    try {
        m_networkWorkers.reserve(networkSources.size());
        for (auto& networkSource : networkSources) {
            NetworkWorker worker;
            worker.appSrc = networkSource.first;
            worker.thread = std::thread(&MediaPlayer::networkWindowInputLoop, this,
                                        worker.appSrc, networkSource.second);
            m_networkWorkers.push_back(std::move(worker));
            networkSource.first = nullptr;
        }
    } catch (const std::exception& error) {
        for (auto& networkSource : networkSources) {
            if (networkSource.first != nullptr) {
                m_runtime->gstObjectUnref(networkSource.first);
                networkSource.first = nullptr;
            }
        }
        setErrorUnlocked(std::string("Cannot start hardware window input: ") + error.what());
        releasePipelineUnlocked();
        return false;
    }

    if (!transitionUnlocked(GST_STATE_PLAYING, m_options.startupTimeoutMs,
                            "display hardware window layout")) {
        releasePipelineUnlocked();
        return false;
    }

    m_state = State::Capture;
    m_lastError.clear();
    std::ostringstream activePlanes;
    for (size_t index = 0; index < planeIds.size(); ++index) {
        if (index != 0) activePlanes << ',';
        activePlanes << planeIds[index];
    }
    LOG_INFO("Hardware DRM window layout active: node=%u sources=%zu layers=%zu "
             "output=%ux%u planes=%s",
             m_nodeId, command.sources.size(), command.layers.size(),
             command.outputWidth, command.outputHeight, activePlanes.str().c_str());
    return true;
}

bool MediaPlayer::showSignalPreview(const SignalSourceConfig& source) {
    std::lock_guard<std::mutex> lock(m_mutex);
    return showSignalUnlocked(source, false);
}

bool MediaPlayer::showWindowLayout(
    const Protocol::SetWindowLayoutCommand& command) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_initialized || !m_runtime) {
        setErrorUnlocked("Media player is not initialized");
        return false;
    }
    if (command.targetNodeId != m_nodeId || command.outputWidth == 0 ||
        command.outputHeight == 0 || command.layers.empty() || command.sources.empty() ||
        command.layers.empty() != command.sources.empty()) {
        setErrorUnlocked("Window layout target or contents are invalid");
        return false;
    }

    // The master layout uses the configured wall dimensions, while each
    // slave renders at its own physical DRM mode. Scale only target edges.
    const Protocol::SetWindowLayoutCommand renderCommand =
        prepareWindowRenderCommandUnlocked(command);

    return showWindowLayoutHardwareUnlocked(renderCommand);
}

bool MediaPlayer::setSignalOutputActive(bool active) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_initialized || m_pipeline == nullptr || m_state != State::Capture) {
        setErrorUnlocked("Signal capture pipeline is not active");
        return false;
    }
    if (m_signalOutputActive == active) return true;

    // The display sink is deliberately built into the raw capture pipeline.
    // Switching it on or off therefore requires rebuilding that pipeline, but
    // this path is only used when a layout first claims/releases the physical
    // screen. Dragging or resizing an existing placement still updates only
    // videocrop and remains live.
    const SignalSourceConfig source = m_signalSource;
    const bool success = showSignalUnlocked(source, active);
    if (!success) {
        setErrorUnlocked(active ? "Cannot enable DRM signal output"
                                : "Cannot disable DRM signal output");
        return false;
    }
    m_lastError.clear();
    LOG_INFO("Signal DRM output %s with raw capture pipeline",
             active ? "enabled" : "disabled");
    return true;
}

bool MediaPlayer::isSignalOutputActive() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_pipeline != nullptr && m_state == State::Capture && m_signalOutputActive;
}

bool MediaPlayer::supportsH264Rtp() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_initialized && !m_rtpEncodeDisabled && m_runtime != nullptr &&
           m_runtime->h264RtpEncodeAvailable();
}

bool MediaPlayer::showSignalUnlocked(const SignalSourceConfig& source, bool outputToDisplay) {
    // This device exposes both DRM planes at the same z-order and its
    // kmssink has no plane-properties support. Keep the background separate
    // while previewing, but release it before physical signal output so it
    // cannot cover the video plane.
    if (outputToDisplay) releaseBackgroundPipelineUnlocked();
    releasePipelineUnlocked();

    if (!m_initialized) {
        setErrorUnlocked("Media player is not initialized");
        return false;
    }
    std::string sourceError;
    if (!Protocol::validateSignalSourceConfig(source, &sourceError) ||
        source.type == SignalSourceType::NONE) {
        setErrorUnlocked(sourceError.empty() ? "Signal source is not configured" : sourceError);
        return false;
    }

    const CaptureOptions capture = captureOptionsForSource(m_options.capture, source);
    const std::string escapedEndpoint = escapePipelineString(source.endpoint);
    if (escapedEndpoint.empty()) {
        setErrorUnlocked("Signal source endpoint contains unsupported control characters");
        return false;
    }

    const std::string escapedAudioCaptureDevice =
        escapePipelineString(capture.audioCaptureDevice);
    std::vector<AudioPlaybackOutput> escapedAudioPlaybackOutputs = selectedAudioOutputs(capture);
    for (AudioPlaybackOutput& playbackOutput : escapedAudioPlaybackOutputs) {
        playbackOutput.device = escapePipelineString(playbackOutput.device);
        if (playbackOutput.device.empty()) {
            setErrorUnlocked("Audio playback device contains unsupported control characters");
            return false;
        }
    }
    if (capture.audioEnabled && escapedAudioCaptureDevice.empty()) {
        setErrorUnlocked("Capture audio device contains unsupported control characters");
        return false;
    }

    const std::string captureCaps = captureCapsDescription(capture);
    const bool networkInput = source.type != SignalSourceType::CAPTURE &&
                              isHttpEndpoint(source.endpoint) &&
                              m_runtime->networkInputAvailable();
    std::ostringstream pipelineDescription;
    if (source.type == SignalSourceType::CAPTURE) {
        pipelineDescription
            << "v4l2src device=\"" << escapedEndpoint
            << "\" io-mode=mmap do-timestamp=true ! "
            << "watchdog timeout=2000 ! " << captureCaps << " ! ";
    } else if (networkInput) {
        pipelineDescription
            << "appsrc name=dms_network_source is-live=false format=bytes do-timestamp=false "
               "block=true leaky-type=downstream max-buffers=2 max-bytes=1048576 "
                "caps=\"image/jpeg\" ! "
                "queue leaky=downstream max-size-buffers=2 ! "
                "jpegdec ! videoconvert ! "
                "video/x-raw,format=BGRx ! queue ! ";
    } else {
        pipelineDescription
            << "uridecodebin uri=\"" << escapedEndpoint
            << "\" name=dms_signal_decode ! queue ! ";
    }
    if (source.width != 0 && source.height != 0) {
        m_signalFrameWidth = source.width;
        m_signalFrameHeight = source.height;
    } else if (source.type != SignalSourceType::CAPTURE) {
        m_signalFrameWidth = 0;
        m_signalFrameHeight = 0;
    }
    const bool addPreviewBranch = m_runtime->previewAvailable();
    RtpEndpoint rtpEndpoint;
    const bool rtpEncode = source.type == SignalSourceType::CAPTURE &&
                           !m_rtpEncodeDisabled &&
                           m_runtime->h264RtpEncodeAvailable() &&
                           parseRtpEndpoint(Protocol::signalStreamEndpoint(m_nodeId), rtpEndpoint);
    // Both Smart windows on the RK3566 report zpos=0 and the primary window
    // can remain owned by fbcon after its background pipeline stops. Use the
    // primary window for physical signal output so it replaces the background
    // instead of being hidden underneath it.
    const uint32_t outputPlaneId = m_options.backgroundPlaneId != 0
        ? m_options.backgroundPlaneId
        : m_options.planeId;
    const uint32_t previewWidth = m_options.outputWidth != 0
        ? m_options.outputWidth
        : 1280;
    const uint32_t previewHeight = m_options.outputHeight != 0
        ? m_options.outputHeight
        : 720;
    // RK628 provides NV61, while videocrop only supports NV12/NV21 among the
    // semi-planar formats. Normalize once before the shared crop so both KMS
    // output and JPEG preview use the same BT.709 color interpretation.
    pipelineDescription
        << "videoconvert name=dms_signal_convert n-threads=4 ! "
        << "video/x-raw,format=NV12 ! ";
    if (addPreviewBranch || rtpEncode) {
        // Keep physical output on the raw capture path. JPEG is only for the
        // network preview; routing it through appsink/appsrc adds avoidable
        // encode, thread and decode latency to the physical display.
        pipelineDescription << "tee name=dms_signal_source_tee ";
        if (rtpEncode) {
            pipelineDescription
                << "dms_signal_source_tee. ! queue leaky=downstream "
                   "max-size-buffers=2 max-size-bytes=0 max-size-time=0 ! "
                   "mpph264enc rc-mode=cbr gop=30 header-mode=each-idr "
                   "bps=8000000 bps-min=7000000 bps-max=9000000 "
                   "profile=main level=40 zero-copy-pkt=true ! "
                   "h264parse config-interval=1 ! "
                   "rtph264pay pt=96 config-interval=1 mtu=1200 "
                   "aggregate-mode=zero-latency ! "
                << "udpsink host=" << rtpEndpoint.host
                << " port=" << rtpEndpoint.port
                << " auto-multicast=true sync=false async=false ";
        }
        if (outputToDisplay) {
            pipelineDescription
                << "dms_signal_source_tee. ! queue leaky=downstream "
                   "max-size-buffers=1 max-size-bytes=0 max-size-time=0 ! "
                   "videocrop name=dms_signal_crop ! "
                   "videoconvert n-threads=4 ! videoscale ! "
                << kmsVideoCaps() << " ! "
                << kmsSinkDescription(false, true, outputPlaneId)
                << (addPreviewBranch ? " dms_signal_source_tee. ! " : "");
        } else if (addPreviewBranch) {
            pipelineDescription << "dms_signal_source_tee. ! ";
        }
        if (addPreviewBranch) {
            pipelineDescription
                << "queue leaky=downstream max-size-buffers=1 max-size-bytes=0 "
                   "max-size-time=0 ! "
                << "videoconvert ! videoscale ! "
                << "video/x-raw,width=" << previewWidth
                << ",height=" << previewHeight
                << ",pixel-aspect-ratio=1/1 ! jpegenc quality=85 ! "
                << "appsink name=dms_preview_sink emit-signals=false sync=false "
                   "max-buffers=1 drop=true";
        } else if (rtpEncode) {
            pipelineDescription
                << "dms_signal_source_tee. ! fakesink sync=false";
        }
    } else if (outputToDisplay) {
        pipelineDescription
            << "videocrop name=dms_signal_crop ! "
            << "videoconvert n-threads=4 ! videoscale ! " << kmsVideoCaps() << " ! "
            << kmsSinkDescription(false, true, outputPlaneId);
    } else if (!addPreviewBranch) {
        pipelineDescription << "videocrop name=dms_signal_crop ! fakesink sync=false";
    }

    GError* parseError = nullptr;
    m_pipeline = m_runtime->gstParseLaunch(pipelineDescription.str().c_str(), &parseError);
    if (m_pipeline == nullptr || parseError != nullptr) {
        const std::string error = m_runtime->consumeError(parseError, "Cannot create signal pipeline");
        if (m_pipeline != nullptr) {
            m_runtime->gstObjectUnref(m_pipeline);
            m_pipeline = nullptr;
        }
        if (rtpEncode) {
            LOG_WARNING("RK MPP H.264 transport unavailable at runtime; falling back to MJPEG: %s",
                        error.c_str());
            m_rtpEncodeDisabled = true;
            return showSignalUnlocked(source, outputToDisplay);
        }
        setErrorUnlocked("Cannot create signal pipeline: " + error);
        return false;
    }

    if (networkInput) {
        m_networkAppSrc = m_runtime->gstBinGetByName(m_pipeline, "dms_network_source");
        if (m_networkAppSrc == nullptr) {
            setErrorUnlocked("Network signal pipeline has no appsrc");
            releasePipelineUnlocked();
            return false;
        }
        // Start the HTTP producer before PLAYING; the worker has a short
        // startup gate so the first JPEG is not pushed during negotiation.
        m_signalSource = source;
        if (!startNetworkInputThreadUnlocked()) {
            setErrorUnlocked("Cannot start HTTP signal input");
            releasePipelineUnlocked();
            return false;
        }
    }

    m_lastError.clear();
    if (!transitionUnlocked(GST_STATE_PLAYING, m_options.startupTimeoutMs, "display signal")) {
        if (rtpEncode) {
            LOG_WARNING("RK MPP H.264 transport failed to start; falling back to MJPEG");
            m_rtpEncodeDisabled = true;
            releasePipelineUnlocked();
            return showSignalUnlocked(source, outputToDisplay);
        }
        releasePipelineUnlocked();
        return false;
    }

    m_state = State::Capture;
    m_signalOutputActive = false;
    m_signalSource = source;
    m_nextCaptureAudioRetry = outputToDisplay
        ? std::chrono::steady_clock::time_point::min()
        : std::chrono::steady_clock::time_point::max();
    if (!startPreviewThreadUnlocked()) {
        LOG_WARNING("Live preview branch is unavailable; physical signal playback remains active");
    }

    // The physical output is already part of the capture pipeline. This keeps
    // the raw frame path intact; later layout updates only change videocrop.
    if (outputToDisplay && addPreviewBranch) {
        if (!startSignalOutputUnlocked()) {
            releasePipelineUnlocked();
            return false;
        }
        m_signalOutputActive = true;
    } else if (outputToDisplay) {
        // Fallback pipeline has a direct KMS sink because no appsink exists.
        m_signalOutputActive = true;
    }

    bool audioStarted = false;
    if (outputToDisplay && source.type == SignalSourceType::CAPTURE && capture.audioEnabled) {
        audioStarted = startCaptureAudioUnlocked(escapedAudioCaptureDevice,
                                                 escapedAudioPlaybackOutputs);
    }
    LOG_INFO("Signal %s: type=%s endpoint=%s caps=%s audio=%s%s%s",
             outputToDisplay ? "displayed" : "preview started",
             signalSourceTypeName(source.type), source.endpoint.c_str(), captureCaps.c_str(),
             audioStarted ? capture.audioCaptureDevice.c_str() : "disabled",
             audioStarted ? " -> " : "",
             audioStarted ? audioOutputModeName(capture.audioOutputMode) : "");
    return true;
}

bool MediaPlayer::playStartupAnimation(const std::string& animationPath, uint32_t timeoutMs) {
    std::lock_guard<std::mutex> lock(m_mutex);
    releasePipelineUnlocked();

    if (!m_initialized) {
        setErrorUnlocked("Media player is not initialized");
        return false;
    }
    if (timeoutMs == 0) {
        setErrorUnlocked("Startup animation timeout must be greater than zero");
        return false;
    }
    if (animationPath.empty() || !std::filesystem::is_regular_file(animationPath)) {
        setErrorUnlocked("Startup animation does not exist: " + animationPath);
        return false;
    }

    const std::string absolutePath = std::filesystem::absolute(animationPath).lexically_normal().string();
    const std::string escapedPath = escapePipelineString(absolutePath);
    if (escapedPath.empty()) {
        setErrorUnlocked("Startup animation path contains unsupported control characters");
        return false;
    }

    // Boot animation replaces the boot/idle image. Smart0-win1 shares hardware
    // with Smart0-win0; overlapping it with the firmware framebuffer can leave
    // a corrupt strip during the initial handoff. Use the background plane
    // exclusively and keep the current display mode when playback ends.
    releaseBackgroundPipelineUnlocked();
    const uint32_t animationPlane = m_options.backgroundPlaneId != 0
        ? m_options.backgroundPlaneId : m_options.planeId;
    std::ostringstream pipelineDescription;
    pipelineDescription
        << "filesrc location=\"" << escapedPath << "\" ! "
        << "parsebin ! "
        << "mppvideodec name=dms_boot_decoder dma-feature=false ! "
        << "videoconvert name=dms_boot_convert ! videoscale ! "
        << kmsVideoCaps() << " ! "
        << kmsSinkDescription(true, false, animationPlane, false);

    LOG_INFO("Startup animation using plane %u, output=%ux%u; keeping display mode",
             animationPlane, m_options.outputWidth, m_options.outputHeight);

    GError* parseError = nullptr;
    m_pipeline = m_runtime->gstParseLaunch(pipelineDescription.str().c_str(), &parseError);
    if (m_pipeline == nullptr || parseError != nullptr) {
        const std::string error = m_runtime->consumeError(parseError, "Cannot create startup animation pipeline");
        if (m_pipeline != nullptr) {
            m_runtime->gstObjectUnref(m_pipeline);
            m_pipeline = nullptr;
        }
        setErrorUnlocked("Cannot create startup animation pipeline: " + error);
        return false;
    }

    m_lastError.clear();
    if (!transitionUnlocked(GST_STATE_PLAYING, m_options.startupTimeoutMs, "start startup animation")) {
        releasePipelineUnlocked();
        return false;
    }
    m_state = State::Playing;

    void* bus = m_runtime->gstElementGetBus(m_pipeline);
    if (bus == nullptr) {
        setErrorUnlocked("Startup animation pipeline has no message bus");
        releasePipelineUnlocked();
        return false;
    }
    void* message = m_runtime->gstBusTimedPopFiltered(
        bus, static_cast<uint64_t>(timeoutMs) * NANOSECONDS_PER_MILLISECOND,
        GST_MESSAGE_ERROR | GST_MESSAGE_EOS);
    bool completed = false;
    if (message == nullptr) {
        setErrorUnlocked("Timed out waiting for startup animation to finish");
    } else {
        const uint32_t messageType = static_cast<GstMessagePrefix*>(message)->type;
        if (messageType == GST_MESSAGE_ERROR) {
            GError* gstError = nullptr;
            char* debugDetails = nullptr;
            m_runtime->gstMessageParseError(message, &gstError, &debugDetails);
            std::string error = m_runtime->consumeError(gstError, "GStreamer startup animation error");
            if (debugDetails != nullptr && debugDetails[0] != '\0') {
                error += " (" + std::string(debugDetails) + ")";
            }
            if (debugDetails != nullptr) {
                m_runtime->gFree(debugDetails);
            }
            setErrorUnlocked(error);
        } else {
            completed = true;
            LOG_INFO("Startup animation completed: %s", absolutePath.c_str());
        }
        m_runtime->gstMiniObjectUnref(message);
    }
    m_runtime->gstObjectUnref(bus);
    if (completed) {
        m_startupPipeline = m_pipeline;
        m_pipeline = nullptr;
        m_state = State::Ended;
        LOG_INFO("Startup animation final frame held until replacement is ready");
    } else {
        releasePipelineUnlocked();
    }
    return completed;
}

bool MediaPlayer::prepare(const std::string& videoPath,
                          uint32_t videoWidth,
                          uint32_t videoHeight,
                          const Protocol::CropRegion& cropRegion) {
    std::lock_guard<std::mutex> lock(m_mutex);
    releasePipelineUnlocked();

    if (!m_initialized) {
        setErrorUnlocked("Media player is not initialized");
        return false;
    }
    if (videoPath.empty() || !std::filesystem::is_regular_file(videoPath)) {
        setErrorUnlocked("Video file does not exist: " + videoPath);
        return false;
    }
    if (videoWidth == 0 || videoHeight == 0 || videoWidth > 65535 || videoHeight > 65535 ||
        cropRegion.nodeId != m_nodeId || cropRegion.cropWidth == 0 || cropRegion.cropHeight == 0 ||
        cropRegion.cropX > std::numeric_limits<uint32_t>::max() - cropRegion.cropWidth ||
        cropRegion.cropY > std::numeric_limits<uint32_t>::max() - cropRegion.cropHeight ||
        cropRegion.cropX + cropRegion.cropWidth > videoWidth ||
        cropRegion.cropY + cropRegion.cropHeight > videoHeight) {
        setErrorUnlocked("Crop region is invalid for this node");
        return false;
    }

    const std::string absolutePath = std::filesystem::absolute(videoPath).lexically_normal().string();
    const std::string escapedPath = escapePipelineString(absolutePath);
    if (escapedPath.empty()) {
        setErrorUnlocked("Video path contains unsupported control characters");
        return false;
    }

    const uint32_t cropRight = videoWidth - cropRegion.cropX - cropRegion.cropWidth;
    const uint32_t cropBottom = videoHeight - cropRegion.cropY - cropRegion.cropHeight;

    std::ostringstream pipelineDescription;
    pipelineDescription
        << "filesrc location=\"" << escapedPath << "\" ! "
        << "parsebin ! "
        << "mppvideodec name=dms_decoder dma-feature=false ! "
        << "videocrop name=dms_crop left=" << cropRegion.cropX
        << " right=" << cropRight
        << " top=" << cropRegion.cropY
        << " bottom=" << cropBottom << " ! "
        << "videoconvert name=dms_convert ! videoscale ! "
        << kmsVideoCaps() << " ! "
        << kmsSinkDescription(true, false, m_options.planeId);

    GError* parseError = nullptr;
    m_pipeline = m_runtime->gstParseLaunch(pipelineDescription.str().c_str(), &parseError);
    if (m_pipeline == nullptr || parseError != nullptr) {
        const std::string error = m_runtime->consumeError(parseError, "Cannot create GStreamer pipeline");
        if (m_pipeline != nullptr) {
            m_runtime->gstObjectUnref(m_pipeline);
            m_pipeline = nullptr;
        }
        setErrorUnlocked("Cannot create RKMPP/KMS pipeline: " + error);
        return false;
    }

    m_lastError.clear();
    if (!transitionUnlocked(GST_STATE_PAUSED, m_options.startupTimeoutMs, "prepare media")) {
        releasePipelineUnlocked();
        return false;
    }

    m_state = State::Prepared;
    LOG_INFO("Media prepared with RKMPP/KMS: file=%s, crop=(%u,%u,%u,%u)",
             absolutePath.c_str(), cropRegion.cropX, cropRegion.cropY,
             cropRegion.cropWidth, cropRegion.cropHeight);
    return true;
}

bool MediaPlayer::play() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!pollBusUnlocked()) {
        return false;
    }
    if (m_pipeline == nullptr || m_state != State::Prepared) {
        setErrorUnlocked("Media is not prepared for initial playback");
        return false;
    }
    if (!transitionUnlocked(GST_STATE_PLAYING, m_options.startupTimeoutMs, "start playback")) {
        return false;
    }
    m_state = State::Playing;
    return true;
}

bool MediaPlayer::pause() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!pollBusUnlocked()) {
        return false;
    }
    if (m_pipeline == nullptr || m_state != State::Playing) {
        setErrorUnlocked("Playback is not active");
        return false;
    }
    if (!transitionUnlocked(GST_STATE_PAUSED, m_options.startupTimeoutMs, "pause playback")) {
        return false;
    }
    m_state = State::Paused;
    return true;
}

bool MediaPlayer::resume() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!pollBusUnlocked()) {
        return false;
    }
    if (m_pipeline == nullptr || m_state != State::Paused) {
        setErrorUnlocked("Playback is not paused");
        return false;
    }
    if (!transitionUnlocked(GST_STATE_PLAYING, m_options.startupTimeoutMs, "resume playback")) {
        return false;
    }
    m_state = State::Playing;
    return true;
}

void MediaPlayer::stop() {
    std::lock_guard<std::mutex> lock(m_mutex);
    releasePipelineUnlocked();
}

bool MediaPlayer::isPrepared() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_pipeline != nullptr &&
           (m_state == State::Prepared || m_state == State::Playing || m_state == State::Paused);
}

bool MediaPlayer::isRunning() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!pollBusUnlocked()) {
        return false;
    }
    return m_pipeline != nullptr &&
           (m_state == State::Prepared || m_state == State::Playing || m_state == State::Paused);
}

bool MediaPlayer::isCaptureRunning() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!pollBusUnlocked()) {
        return false;
    }
    pollCaptureAudioBusUnlocked();
    retryCaptureAudioUnlocked();
    return m_pipeline != nullptr && m_state == State::Capture;
}

bool MediaPlayer::isImageRunning() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_backgroundPipeline != nullptr) {
        return pollPipelineBusUnlocked(m_backgroundPipeline, true) &&
               m_backgroundPipeline != nullptr;
    }
    if (!pollBusUnlocked()) {
        return false;
    }
    return m_pipeline != nullptr && m_state == State::Image;
}

bool MediaPlayer::setCaptureAudioOutputMode(AudioOutputMode mode) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_initialized) {
        setErrorUnlocked("Media player is not initialized");
        return false;
    }

    if (m_options.capture.audioOutputMode == mode) {
        return true;
    }

    m_options.capture.audioOutputMode = mode;
    if (m_state != State::Capture || !m_signalOutputActive ||
        !m_options.capture.audioEnabled) {
        LOG_INFO("Capture audio output mode selected: %s", audioOutputModeName(mode));
        return true;
    }

    const std::string captureDevice =
        escapePipelineString(m_options.capture.audioCaptureDevice);
    std::vector<AudioPlaybackOutput> playbackOutputs =
        selectedAudioOutputs(m_options.capture);
    for (AudioPlaybackOutput& output : playbackOutputs) {
        output.device = escapePipelineString(output.device);
        if (output.device.empty()) {
            setErrorUnlocked("Audio playback device contains unsupported control characters");
            return false;
        }
    }
    if (captureDevice.empty()) {
        setErrorUnlocked("Capture audio device contains unsupported control characters");
        return false;
    }
    return startCaptureAudioUnlocked(captureDevice, playbackOutputs);
}

bool MediaPlayer::setCaptureAudioVolumePercent(uint32_t volumePercent) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_initialized || volumePercent > 100) {
        setErrorUnlocked("Capture audio volume percent must be between 0 and 100");
        return false;
    }
    const uint32_t previousVolumePercent = m_captureAudioVolumePercent;
    m_captureAudioVolumePercent = volumePercent;
    if (m_captureAudioPipeline == nullptr) {
        return true;
    }

    // On the target ALSA/GStreamer stack, a running volume element can remain
    // silent after it has been set to zero. Recreate only the audio branch when
    // unmuting; the raw capture/video output pipeline remains uninterrupted.
    if (previousVolumePercent == 0 && volumePercent > 0 && m_signalOutputActive) {
        const std::string captureDevice =
            escapePipelineString(m_options.capture.audioCaptureDevice);
        std::vector<AudioPlaybackOutput> playbackOutputs =
            selectedAudioOutputs(m_options.capture);
        for (AudioPlaybackOutput& output : playbackOutputs) {
            output.device = escapePipelineString(output.device);
            if (output.device.empty()) {
                setErrorUnlocked("Audio playback device contains unsupported control characters");
                return false;
            }
        }
        if (captureDevice.empty()) {
            setErrorUnlocked("Capture audio device contains unsupported control characters");
            return false;
        }
        if (!startCaptureAudioUnlocked(captureDevice, playbackOutputs)) {
            return false;
        }
        LOG_INFO("Capture audio unmuted at %u%% with audio branch restart", volumePercent);
        return true;
    }

    const std::vector<AudioPlaybackOutput> outputs = selectedAudioOutputs(m_options.capture);
    for (size_t index = 0; index < outputs.size(); ++index) {
        const std::string name = "dms_capture_audio_volume_" + std::to_string(index);
        void* volume = m_runtime->gstBinGetByName(m_captureAudioPipeline, name.c_str());
        if (volume == nullptr) {
            setErrorUnlocked("Capture audio volume element is unavailable: " + name);
            return false;
        }
        const double effectiveVolume = outputs[index].volume * volumePercent / 100.0;
        m_runtime->gObjectSet(volume, "volume", effectiveVolume, nullptr);
        m_runtime->gstObjectUnref(volume);
    }
    LOG_INFO("Capture audio volume set to %u%%", volumePercent);
    return true;
}

std::shared_ptr<const std::vector<uint8_t>> MediaPlayer::getLatestPreviewFrame() const {
    std::lock_guard<std::mutex> lock(m_previewMutex);
    return m_latestPreviewFrame;
}

bool MediaPlayer::startPreviewThreadUnlocked() {
    stopPreviewThreadUnlocked();
    if (m_pipeline == nullptr || !m_runtime->previewAvailable()) return true;
    m_previewSink = m_runtime->gstBinGetByName(m_pipeline, "dms_preview_sink");
    if (m_previewSink == nullptr) {
        if (m_previewSink != nullptr) m_runtime->gstObjectUnref(m_previewSink);
        m_previewSink = nullptr;
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(m_previewMutex);
        m_latestPreviewFrame.reset();
    }
    m_previewRunning.store(true);
    try {
        m_previewThread = std::thread(&MediaPlayer::previewLoop, this);
    } catch (const std::exception& error) {
        m_previewRunning.store(false);
        m_runtime->gstObjectUnref(m_previewSink);
        m_previewSink = nullptr;
        LOG_WARNING("Cannot start live preview thread: %s", error.what());
        return false;
    }
    return true;
}

void MediaPlayer::stopPreviewThreadUnlocked() {
    m_previewRunning.store(false);
    if (m_previewThread.joinable()) m_previewThread.join();
    if (m_previewSink != nullptr) {
        m_runtime->gstObjectUnref(m_previewSink);
        m_previewSink = nullptr;
    }
    std::lock_guard<std::mutex> lock(m_previewMutex);
    m_latestPreviewFrame.reset();
}

bool MediaPlayer::startNetworkInputThreadUnlocked() {
    if (m_networkAppSrc == nullptr || !m_runtime->networkInputAvailable()) {
        return false;
    }

    static std::once_flag curlInitOnce;
    static CURLcode curlInitResult = CURLE_FAILED_INIT;
    std::call_once(curlInitOnce, [] {
        curlInitResult = curl_global_init(CURL_GLOBAL_DEFAULT);
    });
    if (curlInitResult != CURLE_OK) {
        setErrorUnlocked(std::string("Cannot initialize libcurl: ") +
                         curl_easy_strerror(curlInitResult));
        return false;
    }

    m_networkRunning.store(true);
    try {
        m_networkThread = std::thread(&MediaPlayer::networkInputLoop, this);
    } catch (const std::exception& error) {
        m_networkRunning.store(false);
        LOG_WARNING("Cannot start HTTP signal input thread: %s", error.what());
        return false;
    }
    return true;
}

void MediaPlayer::stopNetworkInputThreadUnlocked() {
    m_networkRunning.store(false);
    for (auto& worker : m_networkWorkers) {
        if (worker.appSrc != nullptr && m_runtime->gstAppSrcEndOfStream != nullptr) {
            m_runtime->gstAppSrcEndOfStream(worker.appSrc);
        }
    }
    if (m_networkAppSrc != nullptr && m_runtime->gstAppSrcEndOfStream != nullptr) {
        m_runtime->gstAppSrcEndOfStream(m_networkAppSrc);
    }
    for (auto& worker : m_networkWorkers) {
        if (worker.thread.joinable()) worker.thread.join();
        if (worker.appSrc != nullptr) {
            m_runtime->gstObjectUnref(worker.appSrc);
            worker.appSrc = nullptr;
        }
    }
    m_networkWorkers.clear();
    if (m_networkThread.joinable()) m_networkThread.join();
    if (m_networkAppSrc != nullptr) {
        m_runtime->gstObjectUnref(m_networkAppSrc);
        m_networkAppSrc = nullptr;
    }
}

size_t MediaPlayer::networkWriteCallback(char* data,
                                          size_t size,
                                          size_t count,
                                          void* userData) {
    auto* context = static_cast<NetworkInputContext*>(userData);
    if (context == nullptr || context->player == nullptr || context->pending == nullptr ||
        (count != 0 && size > std::numeric_limits<size_t>::max() / count)) {
        return 0;
    }
    const size_t bytes = size * count;
    return context->player->consumeNetworkBytes(
               reinterpret_cast<const uint8_t*>(data), bytes, *context->pending,
               context->appSrc)
        ? bytes
        : 0;
}

int MediaPlayer::networkProgressCallback(void* userData,
                                         int64_t,
                                         int64_t,
                                         int64_t,
                                         int64_t) {
    auto* context = static_cast<NetworkInputContext*>(userData);
    return context == nullptr || context->player == nullptr ||
                   !context->player->m_networkRunning.load()
        ? 1
        : 0;
}

bool MediaPlayer::consumeNetworkBytes(const uint8_t* data,
                                      size_t size,
                                      std::vector<uint8_t>& pending,
                                      void* appSrc) {
    if (!m_networkRunning.load() || appSrc == nullptr ||
        m_runtime->gstAppSrcPushBuffer == nullptr) return false;
    if (size > MAX_NETWORK_JPEG_SIZE || pending.size() > MAX_NETWORK_JPEG_SIZE - size) {
        pending.clear();
        return false;
    }
    pending.insert(pending.end(), data, data + size);
    const std::array<uint8_t, 2> startMarker{0xffU, 0xd8U};
    const std::array<uint8_t, 2> endMarker{0xffU, 0xd9U};
    while (true) {
        const auto start = std::search(pending.begin(), pending.end(),
                                       startMarker.begin(), startMarker.end());
        if (start == pending.end()) {
            if (pending.size() > 1U && pending.back() == 0xffU) {
                pending.erase(pending.begin(), pending.end() - 1);
            } else {
                pending.clear();
            }
            return true;
        }
        if (start != pending.begin()) pending.erase(pending.begin(), start);
        const auto end = std::search(pending.begin() + 2, pending.end(),
                                     endMarker.begin(), endMarker.end());
        if (end == pending.end()) {
            if (pending.size() > MAX_NETWORK_JPEG_SIZE) {
                pending.clear();
                return false;
            }
            return true;
        }
        const size_t frameSize = static_cast<size_t>(end - pending.begin()) + endMarker.size();
        if (frameSize > MAX_NETWORK_JPEG_SIZE) {
            pending.erase(pending.begin(), pending.begin() + frameSize);
            continue;
        }
        void* buffer = m_runtime->gstBufferNewAllocate(nullptr, frameSize, nullptr);
        if (buffer == nullptr ||
            m_runtime->gstBufferFill(buffer, 0, pending.data(), frameSize) != frameSize) {
            if (buffer != nullptr) m_runtime->gstMiniObjectUnref(buffer);
            return false;
        }
        const int flow = m_runtime->gstAppSrcPushBuffer(appSrc, buffer);
        if (flow < 0) return false;
        pending.erase(pending.begin(), pending.begin() + frameSize);
    }
}

void MediaPlayer::networkInputLoop() {
    const std::string endpoint = m_signalSource.endpoint;
    std::vector<uint8_t> pending;
    // Let the appsrc branch complete its initial state transition before the
    // first JPEG reaches mppjpegdec; this decoder negotiates on that buffer.
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    while (m_networkRunning.load()) {
        CURL* curl = curl_easy_init();
        if (curl == nullptr) {
            LOG_WARNING("Cannot create libcurl handle for HTTP signal input");
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            continue;
        }
        NetworkInputContext context{this, &pending, m_networkAppSrc};
        curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Accept: multipart/x-mixed-replace, image/jpeg");
        curl_easy_setopt(curl, CURLOPT_URL, endpoint.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &MediaPlayer::networkWriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &context);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, &MediaPlayer::networkProgressCallback);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &context);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 3000L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        const CURLcode result = curl_easy_perform(curl);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        pending.clear();
        if (!m_networkRunning.load()) break;
        if (result != CURLE_OK && result != CURLE_WRITE_ERROR) {
            LOG_WARNING("HTTP signal input disconnected: %s", curl_easy_strerror(result));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void MediaPlayer::networkWindowInputLoop(void* appSrc, std::string endpoint) {
    if (appSrc == nullptr || endpoint.empty()) return;

    std::vector<uint8_t> pending;
    // See networkInputLoop(): avoid pushing while the newly-built pipeline is
    // still negotiating its appsrc -> mppjpegdec branch.
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    while (m_networkRunning.load()) {
        CURL* curl = curl_easy_init();
        if (curl == nullptr) {
            LOG_WARNING("Cannot create libcurl handle for window input: %s",
                        endpoint.c_str());
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            continue;
        }

        NetworkInputContext context{this, &pending, appSrc};
        curl_slist* headers = nullptr;
        headers = curl_slist_append(headers,
                                    "Accept: multipart/x-mixed-replace, image/jpeg");
        curl_easy_setopt(curl, CURLOPT_URL, endpoint.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &MediaPlayer::networkWriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &context);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, &MediaPlayer::networkProgressCallback);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &context);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 3000L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        const CURLcode result = curl_easy_perform(curl);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        pending.clear();
        if (!m_networkRunning.load()) break;
        if (result != CURLE_OK && result != CURLE_WRITE_ERROR) {
            LOG_WARNING("Window HTTP input disconnected (%s): %s",
                        endpoint.c_str(), curl_easy_strerror(result));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

bool MediaPlayer::startSignalOutputUnlocked() {
    if (m_pipeline == nullptr) {
        setErrorUnlocked("Signal output pipeline is unavailable");
        return false;
    }
    void* sink = m_runtime->gstBinGetByName(m_pipeline, "dms_sink");
    if (sink == nullptr) {
        setErrorUnlocked("Signal pipeline has no direct DRM output");
        return false;
    }
    m_runtime->gstObjectUnref(sink);
    return true;
}

void MediaPlayer::releaseSignalOutputUnlocked() {
}

void MediaPlayer::previewLoop() {
    while (m_previewRunning.load()) {
        void* previewSample = m_runtime->gstAppSinkTryPullSample(
            m_previewSink, 20ULL * 1000ULL * 1000ULL);
        if (previewSample != nullptr) {
            void* buffer = m_runtime->gstSampleGetBuffer(previewSample);
            if (buffer != nullptr) {
                const size_t size = m_runtime->gstBufferGetSize(buffer);
                if (size > 0 && size <= 8U * 1024U * 1024U) {
                    auto frame = std::make_shared<std::vector<uint8_t>>(size);
                    const size_t copied = m_runtime->gstBufferExtract(
                        buffer, 0, frame->data(), frame->size());
                    if (copied == frame->size()) {
                        std::lock_guard<std::mutex> lock(m_previewMutex);
                        m_latestPreviewFrame = std::move(frame);
                    }
                }
            }
            m_runtime->gstMiniObjectUnref(previewSample);
        }

    }
}

bool MediaPlayer::updateSignalCrop(uint32_t sourceWidth,
                                   uint32_t sourceHeight,
                                   const Protocol::CropRegion& cropRegion) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_initialized || m_pipeline == nullptr || m_state != State::Capture ||
        sourceWidth == 0 || sourceHeight == 0 || cropRegion.nodeId != m_nodeId ||
        cropRegion.cropWidth == 0 || cropRegion.cropHeight == 0 ||
        cropRegion.cropX > sourceWidth - std::min(sourceWidth, cropRegion.cropWidth) ||
        cropRegion.cropY > sourceHeight - std::min(sourceHeight, cropRegion.cropHeight) ||
        cropRegion.cropX + cropRegion.cropWidth > sourceWidth ||
        cropRegion.cropY + cropRegion.cropHeight > sourceHeight) {
        setErrorUnlocked("Signal crop region is invalid for this node");
        return false;
    }
    void* crop = m_runtime->gstBinGetByName(m_pipeline, "dms_signal_crop");
    if (crop == nullptr) {
        setErrorUnlocked("Signal pipeline has no live crop element");
        return false;
    }
    const uint32_t frameWidth = m_signalFrameWidth != 0 ? m_signalFrameWidth : sourceWidth;
    const uint32_t frameHeight = m_signalFrameHeight != 0 ? m_signalFrameHeight : sourceHeight;
    const auto scaleEdge = [](uint32_t value, uint32_t frameSize, uint32_t canvasSize) {
        return static_cast<uint32_t>((static_cast<uint64_t>(value) * frameSize +
                                      canvasSize / 2U) /
                                     canvasSize);
    };
    const uint32_t leftPixels = std::min(
        frameWidth, scaleEdge(cropRegion.cropX, frameWidth, sourceWidth));
    const uint32_t topPixels = std::min(
        frameHeight, scaleEdge(cropRegion.cropY, frameHeight, sourceHeight));
    uint32_t rightEdge = std::min(
        frameWidth,
        scaleEdge(cropRegion.cropX + cropRegion.cropWidth, frameWidth, sourceWidth));
    uint32_t bottomEdge = std::min(
        frameHeight,
        scaleEdge(cropRegion.cropY + cropRegion.cropHeight, frameHeight, sourceHeight));
    if (rightEdge <= leftPixels) rightEdge = std::min(frameWidth, leftPixels + 1U);
    if (bottomEdge <= topPixels) bottomEdge = std::min(frameHeight, topPixels + 1U);
    if (rightEdge <= leftPixels || bottomEdge <= topPixels) {
        m_runtime->gstObjectUnref(crop);
        setErrorUnlocked("Signal crop maps to an empty source region");
        return false;
    }
    const int left = static_cast<int>(leftPixels);
    const int right = static_cast<int>(frameWidth - rightEdge);
    const int top = static_cast<int>(topPixels);
    const int bottom = static_cast<int>(frameHeight - bottomEdge);
    m_runtime->gObjectSet(crop, "left", left, "right", right,
                         "top", top, "bottom", bottom, nullptr);
    m_runtime->gstObjectUnref(crop);
    m_lastError.clear();
    LOG_INFO("Signal crop updated without pipeline restart: node=%u canvas=%ux%u "
             "crop=%u,%u %ux%u source=%ux%u pixels=%u,%u %ux%u",
             cropRegion.nodeId, sourceWidth, sourceHeight,
             cropRegion.cropX, cropRegion.cropY,
             cropRegion.cropWidth, cropRegion.cropHeight,
             frameWidth, frameHeight, leftPixels, topPixels,
             rightEdge - leftPixels, bottomEdge - topPixels);
    return true;
}

uint32_t MediaPlayer::getCaptureAudioVolumePercent() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_captureAudioVolumePercent;
}

AudioOutputMode MediaPlayer::getCaptureAudioOutputMode() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_options.capture.audioOutputMode;
}

std::string MediaPlayer::getLastError() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_lastError;
}

std::string MediaPlayer::kmsSinkDescription(bool sync,
                                            bool showPrerollFrame,
                                            uint32_t planeId,
                                            bool restoreCrtc) const {
    std::ostringstream description;
    description
        << "kmssink name=dms_sink sync=" << (sync ? "true" : "false")
        << " show-preroll-frame=" << (showPrerollFrame ? "true" : "false")
        << " restore-crtc=" << (restoreCrtc ? "true" : "false")
        << " fullscreen=" << (m_options.fullscreen ? "true" : "false");
    description << kmsTargetDescription(planeId);
    return description.str();
}

std::string MediaPlayer::kmsTargetDescription(uint32_t planeId) const {
    std::ostringstream description;
    if (m_options.connectorId != 0) {
        description << " connector-id=" << m_options.connectorId;
    }
    if (planeId != 0) {
        description << " plane-id=" << planeId;
    }
    // kmssink expects a typed GstStructure value here. The structure name is
    // required; without it the property is rejected before the pipeline starts.
    if (planeId == m_options.planeId && m_options.backgroundPlaneId != 0 &&
        m_options.backgroundPlaneId != m_options.planeId) {
        description << " plane-properties=\"application/x-gst-structure,zpos=(int)1\"";
    } else if (planeId == m_options.backgroundPlaneId &&
               m_options.backgroundPlaneId != m_options.planeId) {
        description << " plane-properties=\"application/x-gst-structure,zpos=(int)0\"";
    }
    return description.str();
}

std::string MediaPlayer::kmsWindowSinkDescription(
    const Protocol::WindowLayer& layer, uint32_t planeId, size_t layerIndex) const {
    std::ostringstream description;
    description << "kmssink name=dms_window_sink_" << layerIndex
                << " sync=true show-preroll-frame=true restore-crtc=false"
                << " fullscreen=false";
    if (m_options.connectorId != 0) {
        description << " connector-id=" << m_options.connectorId;
    }
    if (planeId != 0) description << " plane-id=" << planeId;
    // The boot/idle fbcon plane remains enabled at zpos 0 on this driver.
    // Explicitly raise live windows above it; otherwise both planes are
    // active but the background can completely obscure the decoded frame.
    description << " plane-properties=\"application/x-gst-structure,zpos=(int)"
                << static_cast<int>(layerIndex + 1U) << "\"";
    description << " render-rectangle=\"<" << layer.targetX << ',' << layer.targetY
                << ',' << layer.targetWidth << ',' << layer.targetHeight << ">\"";
    return description.str();
}

std::string MediaPlayer::kmsVideoCaps() const {
    std::ostringstream caps;
    caps << KMS_RGB_CAPS;
    if (m_options.outputWidth != 0 && m_options.outputHeight != 0) {
        caps << ",width=" << m_options.outputWidth
             << ",height=" << m_options.outputHeight
             << ",pixel-aspect-ratio=1/1";
    }
    return caps.str();
}

bool MediaPlayer::transitionUnlocked(int targetState, uint32_t timeoutMs, const char* operation) {
    return transitionPipelineUnlocked(m_pipeline, targetState, timeoutMs, operation, false);
}

bool MediaPlayer::transitionPipelineUnlocked(void* pipeline,
                                             int targetState,
                                             uint32_t timeoutMs,
                                             const char* operation,
                                             bool background) {
    if (pipeline == nullptr) {
        setErrorUnlocked(std::string("Cannot ") + operation + ": pipeline is unavailable");
        return false;
    }
    const int setResult = m_runtime->gstElementSetState(pipeline, targetState);
    if (setResult == GST_STATE_CHANGE_FAILURE) {
        pollPipelineBusUnlocked(pipeline, background);
        if (m_lastError.empty()) {
            setErrorUnlocked(std::string("GStreamer failed to ") + operation);
        }
        return false;
    }

    if (setResult == GST_STATE_CHANGE_ASYNC) {
        int currentState = GST_STATE_NULL;
        int pendingState = GST_STATE_NULL;
        const int waitResult = m_runtime->gstElementGetState(
            pipeline, &currentState, &pendingState,
            static_cast<uint64_t>(timeoutMs) * NANOSECONDS_PER_MILLISECOND);
        if (waitResult == GST_STATE_CHANGE_FAILURE || waitResult == GST_STATE_CHANGE_ASYNC) {
            pollPipelineBusUnlocked(pipeline, background);
            if (m_lastError.empty()) {
                setErrorUnlocked(waitResult == GST_STATE_CHANGE_ASYNC
                    ? std::string("Timed out waiting to ") + operation
                    : std::string("GStreamer failed to ") + operation);
            }
            return false;
        }
    }
    return pollPipelineBusUnlocked(pipeline, background);
}

bool MediaPlayer::pollBusUnlocked() {
    return pollPipelineBusUnlocked(m_pipeline, false);
}

bool MediaPlayer::pollPipelineBusUnlocked(void* pipeline, bool background) {
    if (pipeline == nullptr) return background ? false : m_state != State::Failed;

    void* bus = m_runtime->gstElementGetBus(pipeline);
    if (bus == nullptr) {
        setErrorUnlocked("GStreamer pipeline has no message bus");
        if (!background) m_state = State::Failed;
        return false;
    }

    bool healthy = true;
    constexpr uint32_t terminalMessages = GST_MESSAGE_ERROR | GST_MESSAGE_EOS;
    while (void* message = m_runtime->gstBusTimedPopFiltered(bus, 0, terminalMessages)) {
        const uint32_t messageType = static_cast<GstMessagePrefix*>(message)->type;
        if (messageType == GST_MESSAGE_ERROR) {
            GError* gstError = nullptr;
            char* debugDetails = nullptr;
            m_runtime->gstMessageParseError(message, &gstError, &debugDetails);
            std::string error = m_runtime->consumeError(gstError, "GStreamer playback error");
            if (debugDetails != nullptr && debugDetails[0] != '\0') {
                error += " (" + std::string(debugDetails) + ")";
            }
            if (debugDetails != nullptr) {
                m_runtime->gFree(debugDetails);
            }
            setErrorUnlocked(error);
            if (!background) m_state = State::Failed;
        } else {
            if (!background) m_state = State::Ended;
            LOG_INFO("%s playback reached end of stream",
                     background ? "Background" : "Media");
        }
        healthy = false;
        m_runtime->gstMiniObjectUnref(message);
    }
    m_runtime->gstObjectUnref(bus);
    if (!healthy) {
        if (background) {
            releaseBackgroundPipelineUnlocked();
        } else {
            releasePipelineUnlocked();
        }
    }
    return healthy;
}

bool MediaPlayer::startCaptureAudioUnlocked(
    const std::string& captureDevice,
    const std::vector<AudioPlaybackOutput>& playbackOutputs) {
    releaseCaptureAudioPipelineUnlocked();

    if (!m_signalOutputActive) {
        m_nextCaptureAudioRetry = std::chrono::steady_clock::time_point::max();
        LOG_INFO("Capture audio remains stopped while signal output is inactive");
        return false;
    }

    if (playbackOutputs.empty()) {
        scheduleCaptureAudioRetryUnlocked();
        LOG_WARNING("Capture audio unavailable; no playback devices configured");
        return false;
    }

    std::ostringstream description;
    description
        << "alsasrc name=dms_capture_audio_src device=\"" << captureDevice
        << "\" do-timestamp=true ! "
        << "watchdog name=dms_capture_audio_watchdog timeout=3000 ! "
        << "queue name=dms_capture_audio_queue max-size-buffers=0 "
        << "max-size-bytes=0 max-size-time=200000000 leaky=downstream ! "
        << "audioconvert name=dms_capture_audio_convert ! "
        << "audioresample name=dms_capture_audio_resample ! "
        << "audio/x-raw,format=S16LE,rate=48000,channels=2 ! "
        << "tee name=dms_capture_audio_tee ";
    for (size_t index = 0; index < playbackOutputs.size(); ++index) {
        if (index != 0) {
            description << " ";
        }
        description
            << "dms_capture_audio_tee. ! queue name=dms_capture_audio_queue_" << index
            << " max-size-buffers=0 max-size-bytes=0 max-size-time=200000000 leaky=downstream ! "
            << "volume name=dms_capture_audio_volume_" << index << " volume="
            << std::setprecision(8)
            << (playbackOutputs[index].volume * m_captureAudioVolumePercent / 100.0)
            << " ! "
            << "alsasink name=dms_capture_audio_sink_" << index << " device=\""
            << playbackOutputs[index].device << "\" sync=false async=false";
    }

    GError* parseError = nullptr;
    void* audioPipeline = m_runtime->gstParseLaunch(description.str().c_str(), &parseError);
    if (audioPipeline == nullptr || parseError != nullptr) {
        const std::string error = m_runtime->consumeError(
            parseError, "Cannot create capture audio pipeline");
        if (audioPipeline != nullptr) {
            m_runtime->gstObjectUnref(audioPipeline);
        }
        scheduleCaptureAudioRetryUnlocked();
        LOG_WARNING("Capture audio unavailable; retrying while video remains active: %s", error.c_str());
        return false;
    }

    const int setResult = m_runtime->gstElementSetState(audioPipeline, GST_STATE_PLAYING);
    bool started = setResult != GST_STATE_CHANGE_FAILURE;
    if (started && setResult == GST_STATE_CHANGE_ASYNC) {
        int currentState = GST_STATE_NULL;
        int pendingState = GST_STATE_NULL;
        const int waitResult = m_runtime->gstElementGetState(
            audioPipeline, &currentState, &pendingState,
            static_cast<uint64_t>(std::min(m_options.startupTimeoutMs,
                                           CAPTURE_AUDIO_START_TIMEOUT_MS)) *
                NANOSECONDS_PER_MILLISECOND);
        started = waitResult != GST_STATE_CHANGE_FAILURE && waitResult != GST_STATE_CHANGE_ASYNC;
    }

    if (!started) {
        m_runtime->gstElementSetState(audioPipeline, GST_STATE_NULL);
        m_runtime->gstObjectUnref(audioPipeline);
        scheduleCaptureAudioRetryUnlocked();
        LOG_WARNING("Capture audio failed to start; retrying while video remains active");
        return false;
    }

    m_captureAudioPipeline = audioPipeline;
    m_nextCaptureAudioRetry = std::chrono::steady_clock::time_point::max();
    LOG_INFO("Capture audio bridge started: %s -> %s (%zu playback outputs)",
             m_options.capture.audioCaptureDevice.c_str(),
             audioOutputModeName(m_options.capture.audioOutputMode), playbackOutputs.size());
    return true;
}

void MediaPlayer::scheduleCaptureAudioRetryUnlocked() {
    m_nextCaptureAudioRetry = m_signalOutputActive
        ? std::chrono::steady_clock::now() + CAPTURE_AUDIO_RETRY_INTERVAL
        : std::chrono::steady_clock::time_point::max();
}

void MediaPlayer::retryCaptureAudioUnlocked() {
    if (m_state != State::Capture || !m_signalOutputActive ||
        !m_options.capture.audioEnabled ||
        m_captureAudioPipeline != nullptr ||
        std::chrono::steady_clock::now() < m_nextCaptureAudioRetry) {
        return;
    }

    const std::string captureDevice = escapePipelineString(m_options.capture.audioCaptureDevice);
    std::vector<AudioPlaybackOutput> playbackOutputs = selectedAudioOutputs(m_options.capture);
    for (AudioPlaybackOutput& playbackOutput : playbackOutputs) {
        playbackOutput.device = escapePipelineString(playbackOutput.device);
        if (playbackOutput.device.empty()) {
            scheduleCaptureAudioRetryUnlocked();
            return;
        }
    }
    if (captureDevice.empty() || playbackOutputs.empty()) {
        scheduleCaptureAudioRetryUnlocked();
        return;
    }
    startCaptureAudioUnlocked(captureDevice, playbackOutputs);
}

void MediaPlayer::pollCaptureAudioBusUnlocked() {
    if (!m_signalOutputActive) {
        releaseCaptureAudioPipelineUnlocked();
        m_nextCaptureAudioRetry = std::chrono::steady_clock::time_point::max();
        return;
    }
    if (m_captureAudioPipeline == nullptr) {
        return;
    }

    void* bus = m_runtime->gstElementGetBus(m_captureAudioPipeline);
    if (bus == nullptr) {
        LOG_WARNING("Capture audio pipeline has no message bus; video remains active");
        releaseCaptureAudioPipelineUnlocked();
        scheduleCaptureAudioRetryUnlocked();
        return;
    }

    bool stopAudio = false;
    constexpr uint32_t terminalMessages = GST_MESSAGE_ERROR | GST_MESSAGE_EOS;
    while (void* message = m_runtime->gstBusTimedPopFiltered(bus, 0, terminalMessages)) {
        const uint32_t messageType = static_cast<GstMessagePrefix*>(message)->type;
        if (messageType == GST_MESSAGE_ERROR) {
            GError* gstError = nullptr;
            char* debugDetails = nullptr;
            m_runtime->gstMessageParseError(message, &gstError, &debugDetails);
            std::string error = m_runtime->consumeError(gstError, "Capture audio error");
            if (debugDetails != nullptr && debugDetails[0] != '\0') {
                error += " (" + std::string(debugDetails) + ")";
            }
            if (debugDetails != nullptr) {
                m_runtime->gFree(debugDetails);
            }
            LOG_WARNING("Capture audio stopped; video remains active: %s", error.c_str());
        } else {
            LOG_WARNING("Capture audio reached end of stream; video remains active");
        }
        stopAudio = true;
        m_runtime->gstMiniObjectUnref(message);
    }
    m_runtime->gstObjectUnref(bus);

    if (stopAudio) {
        releaseCaptureAudioPipelineUnlocked();
        scheduleCaptureAudioRetryUnlocked();
    }
}

void MediaPlayer::releaseCaptureAudioPipelineUnlocked() {
    if (m_captureAudioPipeline == nullptr) {
        return;
    }
    m_runtime->gstElementSetState(m_captureAudioPipeline, GST_STATE_NULL);
    int currentState = GST_STATE_NULL;
    int pendingState = GST_STATE_NULL;
    m_runtime->gstElementGetState(
        m_captureAudioPipeline, &currentState, &pendingState,
        2ULL * 1000ULL * NANOSECONDS_PER_MILLISECOND);
    m_runtime->gstObjectUnref(m_captureAudioPipeline);
    m_captureAudioPipeline = nullptr;
}

void MediaPlayer::releaseStartupPipelineUnlocked() {
    if (m_startupPipeline == nullptr) return;
    m_runtime->gstElementSetState(m_startupPipeline, GST_STATE_NULL);
    int currentState = GST_STATE_NULL;
    int pendingState = GST_STATE_NULL;
    m_runtime->gstElementGetState(m_startupPipeline, &currentState, &pendingState,
                                 2ULL * 1000ULL * NANOSECONDS_PER_MILLISECOND);
    m_runtime->gstObjectUnref(m_startupPipeline);
    m_startupPipeline = nullptr;
}

void MediaPlayer::releasePipelineUnlocked(bool keepStartupFrame) {
    if (!keepStartupFrame) releaseStartupPipelineUnlocked();
    stopPreviewThreadUnlocked();
    stopNetworkInputThreadUnlocked();
    releaseSignalOutputUnlocked();
    releaseCaptureAudioPipelineUnlocked();
    m_nextCaptureAudioRetry = std::chrono::steady_clock::time_point::max();
    if (m_pipeline != nullptr) {
        m_runtime->gstElementSetState(m_pipeline, GST_STATE_NULL);
        int currentState = GST_STATE_NULL;
        int pendingState = GST_STATE_NULL;
        m_runtime->gstElementGetState(
            m_pipeline, &currentState, &pendingState, 2ULL * 1000ULL * NANOSECONDS_PER_MILLISECOND);
        m_runtime->gstObjectUnref(m_pipeline);
        m_pipeline = nullptr;
    }
    m_state = State::Idle;
    m_rtpEncodeDisabled = false;
    m_signalOutputActive = false;
    m_signalSource = {};
}

void MediaPlayer::releaseBackgroundPipelineUnlocked() {
    if (m_backgroundPipeline == nullptr) return;
    m_runtime->gstElementSetState(m_backgroundPipeline, GST_STATE_NULL);
    int currentState = GST_STATE_NULL;
    int pendingState = GST_STATE_NULL;
    m_runtime->gstElementGetState(
        m_backgroundPipeline, &currentState, &pendingState,
        2ULL * 1000ULL * NANOSECONDS_PER_MILLISECOND);
    m_runtime->gstObjectUnref(m_backgroundPipeline);
    m_backgroundPipeline = nullptr;
    if (!disableDrmPlane(m_options.backgroundPlaneId)) {
        LOG_WARNING("Could not clear DRM background plane %u after stopping kmssink",
                    m_options.backgroundPlaneId);
    }
}

void MediaPlayer::setErrorUnlocked(const std::string& error) {
    m_lastError = error;
    LOG_ERROR("MediaPlayer: %s", error.c_str());
}

} // namespace dms


