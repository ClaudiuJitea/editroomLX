#include "Decoder.h"
#include <QLoggingCategory>
#include <atomic>
#include <cmath>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/display.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

Q_LOGGING_CATEGORY(lcMedia, "editroom.media")

namespace er {

namespace {

struct FormatCloser { void operator()(AVFormatContext* f) const { avformat_close_input(&f); } };
struct CodecCloser { void operator()(AVCodecContext* c) const { avcodec_free_context(&c); } };
struct FrameFree { void operator()(AVFrame* f) const { av_frame_free(&f); } };
struct PacketFree { void operator()(AVPacket* p) const { av_packet_free(&p); } };
using FormatPtr = std::unique_ptr<AVFormatContext, FormatCloser>;
using CodecPtr = std::unique_ptr<AVCodecContext, CodecCloser>;
using FramePtr = std::unique_ptr<AVFrame, FrameFree>;
using PacketPtr = std::unique_ptr<AVPacket, PacketFree>;

FormatPtr openInput(const QString& path) {
    AVFormatContext* f = nullptr;
    if (avformat_open_input(&f, path.toUtf8().constData(), nullptr, nullptr) < 0) return nullptr;
    FormatPtr p(f);
    if (avformat_find_stream_info(f, nullptr) < 0) return nullptr;
    return p;
}

double rotationOf(const AVStream* s) {
    const AVPacketSideData* sd = av_packet_side_data_get(s->codecpar->coded_side_data, s->codecpar->nb_coded_side_data,
                                                          AV_PKT_DATA_DISPLAYMATRIX);
    if (!sd || sd->size < 9 * 4) return 0;
    const double r = av_display_rotation_get(reinterpret_cast<const int32_t*>(sd->data));
    return std::isnan(r) ? 0 : r;
}

SourceColor colorOf(const AVFormatContext* fmt, const AVStream* s) {
    // Apple Log: the Mac reads "LogTransferFunction" from the format description; FFmpeg doesn't surface
    // it, so look for Apple's tag in the stream and file metadata (see docs/PARITY.md).
    auto hasLog = [](const AVDictionary* d) -> int {
        const AVDictionaryEntry* e = nullptr;
        while ((e = av_dict_iterate(d, e))) {
            const QByteArray v = QByteArray(e->value).toLower(), k = QByteArray(e->key).toLower();
            if (v.contains("apple-log") || k.contains("apple-log") || v.contains("applelog")) return v.contains("wide-gamut") || v.contains("log2") ? 2 : 1;
        }
        return 0;
    };
    if (int l = std::max(hasLog(s->metadata), hasLog(fmt->metadata))) return l == 2 ? SourceColor::appleLog2 : SourceColor::appleLog;
    // Dolby Vision (an iPhone's HDR carries a profile 8.4 configuration): converted as macOS does it for those.
    const bool vision = av_packet_side_data_get(s->codecpar->coded_side_data, s->codecpar->nb_coded_side_data, AV_PKT_DATA_DOVI_CONF) != nullptr;
    if (s->codecpar->color_trc == AVCOL_TRC_ARIB_STD_B67) return vision ? SourceColor::hlgVision : SourceColor::hlg;
    if (s->codecpar->color_trc == AVCOL_TRC_SMPTE2084) return vision ? SourceColor::pqVision : SourceColor::pq;
    return SourceColor::standard;
}

double seconds(int64_t ts, AVRational tb) { return ts == AV_NOPTS_VALUE ? NAN : double(ts) * av_q2d(tb); }

} // namespace

void prepareHardwareVideo() {
    // Mesa's Intel driver keeps Vulkan Video decode and encode behind a debug switch (Alder Lake, Mesa 25):
    // turned on for FFmpeg's own Vulkan devices unless the person set ANV_DEBUG themselves.
    static const bool once = [] { if (qgetenv("ANV_DEBUG").isEmpty()) qputenv("ANV_DEBUG", "video-decode,video-encode"); return true; }();
    (void)once;
}

int Probe::orientationForRotation(double r) {
    // FFmpeg's angle is counter-clockwise; the Mac's `.right` (EXIF 6) is a picture turned 90° clockwise.
    double a = std::fmod(std::round(r / 90.0) * 90.0, 360.0);
    if (a < 0) a += 360;
    if (a == 270) return 6;
    if (a == 90) return 8;
    if (a == 180) return 3;
    return 1;
}

std::optional<ProbeResult> Probe::file(const QString& path) {
    auto fmt = openInput(path);
    if (!fmt) return std::nullopt;
    ProbeResult r;
    bool videoStreamPresent = false;
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        AVStream* s = fmt->streams[i];
        const AVCodecParameters* par = s->codecpar;
        if (par->codec_type == AVMEDIA_TYPE_VIDEO && !(s->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
            videoStreamPresent = true;
            if (r.hasVideo) continue;
            const AVCodec* codec = avcodec_find_decoder(par->codec_id);
            if (!codec || par->width <= 0) continue;
            double start = seconds(s->start_time, s->time_base);
            double dur = seconds(s->duration, s->time_base);
            if (std::isnan(start)) start = 0;
            if (std::isnan(dur) || dur <= 0) dur = fmt->duration > 0 ? double(fmt->duration) / AV_TIME_BASE : 0;
            if (!(dur > 0)) continue;
            r.hasVideo = true;
            r.videoStart = start;
            r.videoEnd = start + dur;
            r.orientation = orientationForRotation(rotationOf(s));
            const bool swap = r.orientation >= 5;
            r.width = swap ? par->height : par->width;
            r.height = swap ? par->width : par->height;
            AVRational fr = s->avg_frame_rate.num > 0 ? s->avg_frame_rate : s->r_frame_rate;
            r.fps = fr.num > 0 && fr.den > 0 ? av_q2d(fr) : 0;
            r.color = colorOf(fmt.get(), s);
            r.videoCodec = QString::fromLatin1(codec->name);
        } else if (par->codec_type == AVMEDIA_TYPE_AUDIO && !r.hasAudio) {
            const AVCodec* codec = avcodec_find_decoder(par->codec_id);
            if (!codec) continue;
            double start = seconds(s->start_time, s->time_base);
            double dur = seconds(s->duration, s->time_base);
            if (std::isnan(start)) start = 0;
            if (std::isnan(dur) || dur <= 0) dur = fmt->duration > 0 ? double(fmt->duration) / AV_TIME_BASE : 0;
            if (!(dur > 0)) continue;
            r.hasAudio = true;
            r.audioEnd = start + dur;
            r.audioRate = par->sample_rate;
            r.audioChannels = par->ch_layout.nb_channels;
            r.audioCodec = QString::fromLatin1(codec->name);
        }
    }
    if (!r.hasVideo && !r.hasAudio) return std::nullopt;
    // A file whose video can't be decoded isn't imported as audio (MediaStore.probe).
    if (!r.hasVideo && videoStreamPresent) return std::nullopt;
    r.duration = r.hasVideo ? r.videoEnd : r.audioEnd;
    if (!std::isfinite(r.duration) || r.duration <= 0) return std::nullopt;
    return r;
}

// MARK: Video

struct VideoDecoder::Impl {
    FormatPtr fmt;
    CodecPtr ctx;
    AVBufferRef* hw = nullptr;
    AVPixelFormat hwFormat = AV_PIX_FMT_NONE;
    int stream = -1;
    AVRational tb{1, 1};
    double fps = 30;
    QString path = QStringLiteral("software");
    PacketPtr pkt{av_packet_alloc()};
    FramePtr frame{av_frame_alloc()}, sw{av_frame_alloc()};
    bool eof = false;
    double pos = 0;
    std::optional<VideoFrame> held; // the frame read past the one asked for, by frameAt
    SwsContext* sws = nullptr;
    int matrix = ER_MATRIX_709;
    bool fullRange = false;
    /// VA-API frames handed on as DMA-BUFs (the compositor imports them): how many are out now, and how many may be
    /// at once (the decoder's surfaces are a fixed pool, made that much bigger).
    std::shared_ptr<std::atomic<int>> gpuOut = std::make_shared<std::atomic<int>>(0);
    int gpuBudget = 0;

    ~Impl() {
        if (sws) sws_freeContext(sws);
        ctx.reset();
        if (hw) av_buffer_unref(&hw);
    }

    static AVPixelFormat pick(AVCodecContext* c, const AVPixelFormat* fmts) {
        auto* self = static_cast<Impl*>(c->opaque);
        for (const AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; ++p)
            if (*p == self->hwFormat) return *p;
        // The hardware can't take this stream after all: software.
        self->path = QStringLiteral("software");
        for (const AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; ++p) {
            const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(*p);
            if (d && !(d->flags & AV_PIX_FMT_FLAG_HWACCEL)) return *p;
        }
        return fmts[0];
    }

    void colours(const AVFrame* f) {
        switch (f->colorspace) {
        case AVCOL_SPC_BT709: matrix = ER_MATRIX_709; break;
        case AVCOL_SPC_BT470BG: case AVCOL_SPC_SMPTE170M: matrix = ER_MATRIX_601; break;
        case AVCOL_SPC_BT2020_NCL: case AVCOL_SPC_BT2020_CL: matrix = ER_MATRIX_2020; break;
        default: matrix = f->height >= 720 ? ER_MATRIX_709 : ER_MATRIX_601; break;
        }
        fullRange = f->color_range == AVCOL_RANGE_JPEG || f->format == AV_PIX_FMT_YUVJ420P;
    }

    /// A VA-API frame as DMA-BUFs, with nothing copied: none when it can't be (then it's downloaded as before).
    std::optional<VideoFrame> exported(AVFrame* f) {
        if (gpuBudget <= 0 || gpuOut->load() >= gpuBudget) return std::nullopt;
        AVFrame* drm = av_frame_alloc();
        drm->format = AV_PIX_FMT_DRM_PRIME;
        if (av_hwframe_map(drm, f, AV_HWFRAME_MAP_READ) < 0) { av_frame_free(&drm); return std::nullopt; }
        const auto* d = reinterpret_cast<const AVDRMFrameDescriptor*>(drm->data[0]);
        // Luma and interleaved chroma, one plane each: R8 + GR88 (NV12) or R16 + GR32 (P010).
        const uint32_t r8 = 0x20203852, gr88 = 0x38385247, r16 = 0x20363152, gr32 = 0x32335247;
        if (d->nb_layers != 2 || d->layers[0].nb_planes != 1 || d->layers[1].nb_planes != 1) { av_frame_free(&drm); return std::nullopt; }
        const bool wide = d->layers[0].format == r16 && d->layers[1].format == gr32;
        if (!wide && !(d->layers[0].format == r8 && d->layers[1].format == gr88)) { av_frame_free(&drm); return std::nullopt; }
        colours(f);
        VideoFrame out;
        out.format = wide ? ER_SRC_P010 : ER_SRC_NV12;
        out.width = f->width;
        out.height = f->height;
        out.matrix = matrix;
        out.fullRange = fullRange;
        VideoFrame::tightLayout(out.format, out.width, out.height, out.offset, out.stride);
        out.key = VideoFrame::nextKey();
        const int64_t ts = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
        out.time = seconds(ts, tb);
        if (std::isnan(out.time)) out.time = pos;
        auto buf = std::make_shared<DmaBuf>();
        for (int l = 0; l < 2; ++l) {
            const auto& plane = d->layers[l].planes[0];
            const auto& obj = d->objects[plane.object_index];
            DmaBuf::Layer L;
            L.fd = obj.fd;
            L.fourcc = d->layers[l].format;
            L.modifier = obj.format_modifier;
            L.offset = plane.offset;
            L.pitch = plane.pitch;
            L.width = l == 0 ? f->width : (f->width + 1) / 2;
            L.height = l == 0 ? f->height : (f->height + 1) / 2;
            L.bytesPerTexel = (l == 0 ? 1 : 2) * (wide ? 2 : 1);
            buf->layers.push_back(L);
        }
        // The mapping keeps the surface; the counter makes room for the next one when it goes.
        ++*gpuOut;
        auto out_ = gpuOut;
        buf->hold = std::shared_ptr<void>(drm, [out_](void* p) { AVFrame* fr = static_cast<AVFrame*>(p); av_frame_free(&fr); --*out_; });
        // Downloaded only if something on the CPU asks for the pixels.
        AVFrame* src = av_frame_clone(f);
        std::shared_ptr<AVFrame> keep(src, [](AVFrame* p) { av_frame_free(&p); });
        const int format = out.format, w = out.width, h = out.height, mx = matrix;
        const bool fr = fullRange;
        buf->download = [keep, format, w, h, mx, fr]() -> std::shared_ptr<const std::vector<uint8_t>> {
            FramePtr sw(av_frame_alloc());
            if (av_hwframe_transfer_data(sw.get(), keep.get(), 0) < 0) return nullptr;
            const uint8_t* planes[3] = {sw->data[0], sw->data[1], sw->data[2]};
            const int strides[3] = {sw->linesize[0], sw->linesize[1], sw->linesize[2]};
            return VideoFrame::yuv(format, w, h, planes, strides, mx, fr).data;
        };
        out.dmabuf = buf;
        return out;
    }

    VideoFrame wrap(AVFrame* f) {
        // Colour attachments, as Core Image reads them from the pixel buffer.
        switch (f->colorspace) {
        case AVCOL_SPC_BT709: matrix = ER_MATRIX_709; break;
        case AVCOL_SPC_BT470BG: case AVCOL_SPC_SMPTE170M: matrix = ER_MATRIX_601; break;
        case AVCOL_SPC_BT2020_NCL: case AVCOL_SPC_BT2020_CL: matrix = ER_MATRIX_2020; break;
        default: matrix = f->height >= 720 ? ER_MATRIX_709 : ER_MATRIX_601; break;
        }
        fullRange = f->color_range == AVCOL_RANGE_JPEG || f->format == AV_PIX_FMT_YUVJ420P;
        int format = -1;
        switch (f->format) {
        case AV_PIX_FMT_NV12: format = ER_SRC_NV12; break;
        case AV_PIX_FMT_P010LE: format = ER_SRC_P010; break;
        case AV_PIX_FMT_YUV420P: case AV_PIX_FMT_YUVJ420P: format = ER_SRC_YUV420P; break;
        case AV_PIX_FMT_YUV420P10LE: format = ER_SRC_YUV420P10; break;
        default: break;
        }
        AVFrame* src = f;
        FramePtr converted;
        if (format < 0) {
            // Anything else (4:2:2, 4:4:4, 12-bit, RGB…) is repacked to 4:2:0 without changing its colours.
            const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(AVPixelFormat(f->format));
            const bool wide = d && d->comp[0].depth > 8;
            const AVPixelFormat target = wide ? AV_PIX_FMT_P010LE : AV_PIX_FMT_NV12;
            sws = sws_getCachedContext(sws, f->width, f->height, AVPixelFormat(f->format), f->width, f->height, target,
                                       SWS_BILINEAR, nullptr, nullptr, nullptr);
            converted.reset(av_frame_alloc());
            converted->format = target;
            converted->width = f->width;
            converted->height = f->height;
            av_frame_get_buffer(converted.get(), 32);
            sws_scale(sws, f->data, f->linesize, 0, f->height, converted->data, converted->linesize);
            src = converted.get();
            format = wide ? ER_SRC_P010 : ER_SRC_NV12;
        }
        const uint8_t* planes[3] = {src->data[0], src->data[1], src->data[2]};
        const int strides[3] = {src->linesize[0], src->linesize[1], src->linesize[2]};
        VideoFrame out = VideoFrame::yuv(format, src->width, src->height, planes, strides, matrix, fullRange);
        const int64_t ts = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
        out.time = seconds(ts, tb);
        if (std::isnan(out.time)) out.time = pos;
        return out;
    }

    std::optional<VideoFrame> decodeNext() {
        while (true) {
            int r = avcodec_receive_frame(ctx.get(), frame.get());
            if (r == 0) {
                AVFrame* f = frame.get();
                if (f->format == hwFormat && hwFormat == AV_PIX_FMT_VAAPI) {
                    if (auto v = exported(f)) {
                        av_frame_unref(frame.get());
                        pos = v->time;
                        return v;
                    }
                }
                if (f->format == hwFormat && hwFormat != AV_PIX_FMT_NONE) {
                    av_frame_unref(sw.get());
                    if (av_hwframe_transfer_data(sw.get(), f, 0) < 0) { av_frame_unref(f); continue; }
                    av_frame_copy_props(sw.get(), f);
                    f = sw.get();
                }
                VideoFrame v = wrap(f);
                av_frame_unref(frame.get());
                pos = v.time;
                return v;
            }
            if (r == AVERROR_EOF) return std::nullopt;
            if (r != AVERROR(EAGAIN)) return std::nullopt;
            if (eof) return std::nullopt;
            // Feed it.
            while (true) {
                r = av_read_frame(fmt.get(), pkt.get());
                if (r < 0) {
                    eof = true;
                    avcodec_send_packet(ctx.get(), nullptr);
                    break;
                }
                if (pkt->stream_index != stream) { av_packet_unref(pkt.get()); continue; }
                avcodec_send_packet(ctx.get(), pkt.get());
                av_packet_unref(pkt.get());
                break;
            }
        }
    }
};

VideoDecoder::VideoDecoder() : d(std::make_unique<Impl>()) {}
VideoDecoder::~VideoDecoder() = default;

std::unique_ptr<VideoDecoder> VideoDecoder::open(const QString& file, Accel accel, QString* why) {
    std::unique_ptr<VideoDecoder> v(new VideoDecoder());
    Impl& I = *v->d;
    I.fmt = openInput(file);
    if (!I.fmt) { if (why) *why = QStringLiteral("can't open"); return nullptr; }
    const AVCodec* codec = nullptr;
    I.stream = av_find_best_stream(I.fmt.get(), AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (I.stream < 0 || !codec) { if (why) *why = QStringLiteral("no video"); return nullptr; }
    AVStream* s = I.fmt->streams[I.stream];
    I.tb = s->time_base;
    AVRational fr = s->avg_frame_rate.num > 0 ? s->avg_frame_rate : s->r_frame_rate;
    I.fps = fr.num > 0 && fr.den > 0 ? av_q2d(fr) : 30;
    for (unsigned i = 0; i < I.fmt->nb_streams; ++i) if (int(i) != I.stream) I.fmt->streams[i]->discard = AVDISCARD_ALL;
    I.ctx.reset(avcodec_alloc_context3(codec));
    avcodec_parameters_to_context(I.ctx.get(), s->codecpar);
    I.ctx->pkt_timebase = s->time_base;
    I.ctx->opaque = &I;
    prepareHardwareVideo();
    const QByteArray env = qgetenv("EDITROOM_HWDEC");
    // Hardware decoders only get the profiles they really handle: some accept others (lossless H.264
    // High 4:4:4, 4:2:2) and return garbage instead of failing.
    const AVCodecParameters* par = s->codecpar;
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(AVPixelFormat(par->format));
    const bool is420 = !desc || (desc->log2_chroma_w == 1 && desc->log2_chroma_h == 1);
    bool hwProfile = false;
    switch (par->codec_id) {
    case AV_CODEC_ID_H264: hwProfile = par->profile == AV_PROFILE_H264_BASELINE || par->profile == AV_PROFILE_H264_CONSTRAINED_BASELINE ||
                                       par->profile == AV_PROFILE_H264_MAIN || par->profile == AV_PROFILE_H264_HIGH; break;
    case AV_CODEC_ID_HEVC: hwProfile = par->profile == AV_PROFILE_HEVC_MAIN || par->profile == AV_PROFILE_HEVC_MAIN_10; break;
    case AV_CODEC_ID_VP9: case AV_CODEC_ID_AV1: hwProfile = true; break;
    default: break;
    }
    if (accel == Accel::automatic && env != "software" && env != "none" && hwProfile && is420) {
        const AVHWDeviceType order[] = {AV_HWDEVICE_TYPE_VAAPI, AV_HWDEVICE_TYPE_VULKAN, AV_HWDEVICE_TYPE_CUDA};
        for (AVHWDeviceType type : order) {
            if (!env.isEmpty() && env != av_hwdevice_get_type_name(type)) continue;
            AVPixelFormat pf = AV_PIX_FMT_NONE;
            for (int i = 0;; ++i) {
                const AVCodecHWConfig* cfg = avcodec_get_hw_config(codec, i);
                if (!cfg) break;
                if ((cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) && cfg->device_type == type) { pf = cfg->pix_fmt; break; }
            }
            if (pf == AV_PIX_FMT_NONE) continue;
            AVBufferRef* dev = nullptr;
            if (av_hwdevice_ctx_create(&dev, type, nullptr, nullptr, 0) < 0) continue;
            I.hw = dev;
            I.hwFormat = pf;
            I.ctx->hw_device_ctx = av_buffer_ref(dev);
            I.ctx->get_format = &Impl::pick;
            // VA-API frames go on as DMA-BUFs (EDITROOM_DMABUF=0: always downloaded). The surfaces are a fixed pool:
            // as many more as may be out at once, about 384 MB of them, 4 to 32.
            if (type == AV_HWDEVICE_TYPE_VAAPI && qEnvironmentVariable("EDITROOM_DMABUF") != QLatin1String("0") && par->width > 0 && par->height > 0) {
                const bool wide10 = desc && desc->comp[0].depth > 8;
                const double frameBytes = double(par->width) * par->height * 1.5 * (wide10 ? 2 : 1);
                I.gpuBudget = std::clamp(int(384e6 / frameBytes), 4, 32);
                I.ctx->extra_hw_frames = I.gpuBudget + 2;
            }
            I.path = QString::fromLatin1(av_hwdevice_get_type_name(type));
            break;
        }
    }
    I.ctx->thread_count = 0; // software decode uses every core
    if (avcodec_open2(I.ctx.get(), codec, nullptr) < 0) {
        // The hardware refused it: try once more in software.
        if (I.hw) return open(file, Accel::software, why);
        if (why) *why = QStringLiteral("can't decode");
        return nullptr;
    }
    qCDebug(lcMedia) << "decoding" << file << "with" << I.path;
    return v;
}

bool VideoDecoder::seek(double t) {
    Impl& I = *d;
    const int64_t ts = int64_t(std::floor(t / av_q2d(I.tb)));
    if (av_seek_frame(I.fmt.get(), I.stream, ts, AVSEEK_FLAG_BACKWARD) < 0) return false;
    avcodec_flush_buffers(I.ctx.get());
    I.eof = false;
    I.held.reset();
    I.pos = t;
    return true;
}

std::optional<VideoFrame> VideoDecoder::next() {
    if (d->held) { auto h = d->held; d->held.reset(); return h; }
    return d->decodeNext();
}

std::optional<VideoFrame> VideoDecoder::frameAt(double t) {
    std::optional<VideoFrame> last;
    while (auto f = next()) {
        if (f->time > t + 1e-6) {
            if (!last) return f; // the first frame starts after t: it's the one showing
            d->held = f;
            return last;
        }
        last = f;
    }
    return last;
}

QString VideoDecoder::path() const { return d->path; }
double VideoDecoder::fps() const { return d->fps; }
double VideoDecoder::position() const { return d->pos; }
int VideoDecoder::width() const { return d->ctx->width; }
int VideoDecoder::height() const { return d->ctx->height; }

// MARK: Audio

struct AudioDecoder::Impl {
    FormatPtr fmt;
    CodecPtr ctx;
    SwrContext* swr = nullptr;
    int stream = -1;
    int rate = 48000;
    AVRational tb{1, 1};
    PacketPtr pkt{av_packet_alloc()};
    FramePtr frame{av_frame_alloc()};
    bool eof = false;
    std::vector<float> pending;
    double duration = 0;
    /// Frames still to drop after a seek, so reading starts exactly where asked.
    int64_t skip = 0;
    double seekTarget = -1;
    ~Impl() { if (swr) swr_free(&swr); }

    bool refill() {
        while (true) {
            int r = avcodec_receive_frame(ctx.get(), frame.get());
            if (r == 0) {
                const int outMax = int(av_rescale_rnd(swr_get_delay(swr, frame->sample_rate) + frame->nb_samples, rate, frame->sample_rate, AV_ROUND_UP));
                std::vector<float> buf(size_t(outMax) * 2);
                uint8_t* out[1] = {reinterpret_cast<uint8_t*>(buf.data())};
                if (seekTarget >= 0) {
                    const double ft = seconds(frame->best_effort_timestamp, tb);
                    skip = std::isnan(ft) ? 0 : std::max<int64_t>(0, int64_t(std::llround((seekTarget - ft) * rate)));
                    seekTarget = -1;
                }
                const int n = swr_convert(swr, out, outMax, const_cast<const uint8_t**>(frame->extended_data), frame->nb_samples);
                av_frame_unref(frame.get());
                if (n > 0) {
                    int start = 0;
                    if (skip > 0) { start = int(std::min<int64_t>(skip, n)); skip -= start; }
                    pending.insert(pending.end(), buf.begin() + start * 2, buf.begin() + n * 2);
                    return true;
                }
                continue;
            }
            if (r == AVERROR_EOF || (r != AVERROR(EAGAIN))) {
                // Whatever the resampler still holds.
                std::vector<float> buf(4096 * 2);
                uint8_t* out[1] = {reinterpret_cast<uint8_t*>(buf.data())};
                const int n = swr_convert(swr, out, 4096, nullptr, 0);
                if (n > 0) { pending.insert(pending.end(), buf.begin(), buf.begin() + n * 2); return true; }
                return false;
            }
            if (eof) { avcodec_send_packet(ctx.get(), nullptr); continue; }
            while (true) {
                r = av_read_frame(fmt.get(), pkt.get());
                if (r < 0) { eof = true; avcodec_send_packet(ctx.get(), nullptr); break; }
                if (pkt->stream_index != stream) { av_packet_unref(pkt.get()); continue; }
                avcodec_send_packet(ctx.get(), pkt.get());
                av_packet_unref(pkt.get());
                break;
            }
        }
    }
};

AudioDecoder::AudioDecoder() : d(std::make_unique<Impl>()) {}
AudioDecoder::~AudioDecoder() = default;

std::unique_ptr<AudioDecoder> AudioDecoder::open(const QString& path, int rate, QString* why) {
    std::unique_ptr<AudioDecoder> a(new AudioDecoder());
    Impl& I = *a->d;
    I.rate = rate;
    I.fmt = openInput(path);
    if (!I.fmt) { if (why) *why = QStringLiteral("can't open"); return nullptr; }
    const AVCodec* codec = nullptr;
    I.stream = av_find_best_stream(I.fmt.get(), AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
    if (I.stream < 0 || !codec) { if (why) *why = QStringLiteral("no audio"); return nullptr; }
    for (unsigned i = 0; i < I.fmt->nb_streams; ++i) if (int(i) != I.stream) I.fmt->streams[i]->discard = AVDISCARD_ALL;
    AVStream* s = I.fmt->streams[I.stream];
    I.tb = s->time_base;
    I.duration = s->duration != AV_NOPTS_VALUE ? seconds(s->duration, s->time_base) : double(I.fmt->duration) / AV_TIME_BASE;
    I.ctx.reset(avcodec_alloc_context3(codec));
    avcodec_parameters_to_context(I.ctx.get(), s->codecpar);
    I.ctx->pkt_timebase = s->time_base;
    if (avcodec_open2(I.ctx.get(), codec, nullptr) < 0) { if (why) *why = QStringLiteral("can't decode"); return nullptr; }
    AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
    AVChannelLayout in = I.ctx->ch_layout;
    if (in.nb_channels == 0) av_channel_layout_default(&in, 2);
    if (swr_alloc_set_opts2(&I.swr, &stereo, AV_SAMPLE_FMT_FLT, rate, &in, I.ctx->sample_fmt, I.ctx->sample_rate, 0, nullptr) < 0 ||
        swr_init(I.swr) < 0) {
        if (why) *why = QStringLiteral("can't resample");
        return nullptr;
    }
    return a;
}

bool AudioDecoder::seek(double t) {
    Impl& I = *d;
    const int64_t ts = int64_t(std::floor(std::max(0.0, t) / av_q2d(I.tb)));
    if (av_seek_frame(I.fmt.get(), I.stream, ts, AVSEEK_FLAG_BACKWARD) < 0) return false;
    avcodec_flush_buffers(I.ctx.get());
    swr_init(I.swr);
    I.eof = false;
    I.pending.clear();
    I.seekTarget = std::max(0.0, t);
    return true;
}

int AudioDecoder::read(std::vector<float>& out, int frames) {
    Impl& I = *d;
    while (int(I.pending.size() / 2) < frames && I.refill()) {}
    const int n = std::min(frames, int(I.pending.size() / 2));
    out.insert(out.end(), I.pending.begin(), I.pending.begin() + n * 2);
    I.pending.erase(I.pending.begin(), I.pending.begin() + n * 2);
    return n;
}

std::vector<float> AudioDecoder::readRange(double t0, double t1) {
    std::vector<float> out;
    if (t0 > 0) seek(t0);
    const int64_t want = int64_t(std::llround((t1 - t0) * d->rate));
    while (int64_t(out.size() / 2) < want) {
        if (read(out, int(std::min<int64_t>(65536, want - int64_t(out.size() / 2)))) == 0) break;
    }
    return out;
}

int AudioDecoder::rate() const { return d->rate; }
double AudioDecoder::duration() const { return d->duration; }

} // namespace er
