#include "MediaWriter.h"
#include "media/Decoder.h"
#include <QFile>
#include <QLoggingCategory>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
}

Q_LOGGING_CATEGORY(lcExport, "editroom.export")

namespace {
/// Whether a hardware encoder keeps colour: one frame of flat red (and one of blue) encoded with it and decoded in
/// software, the chroma compared. Intel's Vulkan Video encode (still experimental in Mesa) has been seen to keep
/// brightness but scramble colour; an encoder that does is not used. Checked once per encoder.
bool keepsColour(const char* name, AVHWDeviceType hw, bool hevc) {
    static std::mutex m;
    static std::map<QByteArray, bool> known;
    std::lock_guard g(m);
    if (auto it = known.find(name); it != known.end()) return it->second;
    bool ok = false;
    const int w = 256, h = 144;
    const AVCodec* codec = avcodec_find_encoder_by_name(name);
    const AVCodec* dcodec = avcodec_find_decoder(hevc ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264);
    AVCodecContext* ctx = codec ? avcodec_alloc_context3(codec) : nullptr;
    AVCodecContext* dec = dcodec ? avcodec_alloc_context3(dcodec) : nullptr;
    AVBufferRef* dev = nullptr;
    AVBufferRef* frames = nullptr;
    AVFrame* sw = av_frame_alloc();
    AVFrame* hwf = av_frame_alloc();
    AVFrame* out = av_frame_alloc();
    AVPacket* pkt = av_packet_alloc();
    // Two frames: red (Y 63, U 102, V 240) then blue (Y 32, U 240, V 118), BT.709 limited range.
    const int Y[2] = {63, 32}, U[2] = {102, 240}, V[2] = {240, 118};
    int seen = 0, matched = 0;
    do {
        if (!ctx || !dec) break;
        ctx->width = w;
        ctx->height = h;
        ctx->time_base = AVRational{1, 30};
        ctx->framerate = AVRational{30, 1};
        ctx->bit_rate = 2'000'000;
        ctx->gop_size = 1;
        ctx->max_b_frames = 0;
        ctx->colorspace = AVCOL_SPC_BT709;
        ctx->color_range = AVCOL_RANGE_MPEG;
        if (av_hwdevice_ctx_create(&dev, hw, nullptr, nullptr, 0) < 0) break;
        frames = av_hwframe_ctx_alloc(dev);
        auto* fc = reinterpret_cast<AVHWFramesContext*>(frames->data);
        fc->format = hw == AV_HWDEVICE_TYPE_VAAPI ? AV_PIX_FMT_VAAPI : AV_PIX_FMT_VULKAN;
        fc->sw_format = AV_PIX_FMT_NV12;
        fc->width = w;
        fc->height = h;
        fc->initial_pool_size = 4;
        if (av_hwframe_ctx_init(frames) < 0) break;
        ctx->pix_fmt = fc->format;
        ctx->hw_frames_ctx = av_buffer_ref(frames);
        if (avcodec_open2(ctx, codec, nullptr) < 0) break;
        if (avcodec_open2(dec, dcodec, nullptr) < 0) break;
        sw->format = AV_PIX_FMT_NV12;
        sw->width = w;
        sw->height = h;
        if (av_frame_get_buffer(sw, 32) < 0) break;
        auto drain = [&] {
            while (avcodec_receive_packet(ctx, pkt) == 0) {
                avcodec_send_packet(dec, pkt);
                av_packet_unref(pkt);
                while (avcodec_receive_frame(dec, out) == 0) {
                    if (seen < 2 && out->format == AV_PIX_FMT_YUV420P) {
                        const int u = out->data[1][(h / 4) * out->linesize[1] + w / 4], v = out->data[2][(h / 4) * out->linesize[2] + w / 4];
                        if (std::abs(u - U[seen]) <= 12 && std::abs(v - V[seen]) <= 12) ++matched;
                    }
                    ++seen;
                    av_frame_unref(out);
                }
            }
        };
        for (int f = 0; f < 2; ++f) {
            av_frame_make_writable(sw);
            for (int y = 0; y < h; ++y) std::memset(sw->data[0] + y * sw->linesize[0], Y[f], size_t(w));
            for (int y = 0; y < h / 2; ++y)
                for (int x = 0; x < w / 2; ++x) { sw->data[1][y * sw->linesize[1] + 2 * x] = uint8_t(U[f]); sw->data[1][y * sw->linesize[1] + 2 * x + 1] = uint8_t(V[f]); }
            sw->pts = f;
            av_frame_unref(hwf);
            if (av_hwframe_get_buffer(frames, hwf, 0) < 0 || av_hwframe_transfer_data(hwf, sw, 0) < 0) break;
            hwf->pts = f;
            if (avcodec_send_frame(ctx, hwf) < 0) break;
            drain();
        }
        avcodec_send_frame(ctx, nullptr);
        drain();
        avcodec_send_packet(dec, nullptr);
        while (avcodec_receive_frame(dec, out) == 0) {
            if (seen < 2 && out->format == AV_PIX_FMT_YUV420P) {
                const int u = out->data[1][(h / 4) * out->linesize[1] + w / 4], v = out->data[2][(h / 4) * out->linesize[2] + w / 4];
                if (std::abs(u - U[seen]) <= 12 && std::abs(v - V[seen]) <= 12) ++matched;
            }
            ++seen;
            av_frame_unref(out);
        }
        ok = seen >= 2 && matched == 2;
    } while (false);
    av_packet_free(&pkt);
    av_frame_free(&out);
    av_frame_free(&hwf);
    av_frame_free(&sw);
    avcodec_free_context(&dec);
    avcodec_free_context(&ctx);
    av_buffer_unref(&frames);
    av_buffer_unref(&dev);
    known[name] = ok;
    if (!ok) qCWarning(lcExport) << name << "loses colour on this computer, so it isn't used";
    return ok;
}
} // namespace

namespace er {

std::pair<int, int> frameDuration(double fps) {
    const double rounded = std::round(fps);
    if (std::abs(fps - rounded) < 0.001) return {1, int(rounded)};
    // NTSC style rates: 23.976, 29.97, 59.94
    return {1001, int(std::round(fps * 1.001)) * 1000};
}

struct MediaWriter::Impl {
    AVFormatContext* fmt = nullptr;
    AVCodecContext* venc = nullptr;
    AVCodecContext* aenc = nullptr;
    AVStream* vst = nullptr;
    AVStream* ast = nullptr;
    AVBufferRef* hwDevice = nullptr;
    AVBufferRef* hwFrames = nullptr;
    AVFrame* vframe = nullptr;
    AVFrame* hwframe = nullptr;
    AVFrame* aframe = nullptr;
    AVPacket* pkt = av_packet_alloc();
    QString encoder;
    std::vector<float> pendingAudio; // interleaved, waiting for a full encoder frame
    int64_t audioPts = 0;
    bool headerWritten = false;
    bool hdr = false;
    QString path;

    ~Impl() {
        av_frame_free(&vframe);
        av_frame_free(&hwframe);
        av_frame_free(&aframe);
        av_packet_free(&pkt);
        avcodec_free_context(&venc);
        avcodec_free_context(&aenc);
        av_buffer_unref(&hwFrames);
        av_buffer_unref(&hwDevice);
        if (fmt) {
            if (!(fmt->oformat->flags & AVFMT_NOFILE)) avio_closep(&fmt->pb);
            avformat_free_context(fmt);
        }
    }

    bool drain(AVCodecContext* c, AVStream* st) {
        while (true) {
            const int r = avcodec_receive_packet(c, pkt);
            if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) return true;
            if (r < 0) return false;
            av_packet_rescale_ts(pkt, c->time_base, st->time_base);
            pkt->stream_index = st->index;
            if (av_interleaved_write_frame(fmt, pkt) < 0) return false;
        }
    }

    bool openVideo(const WriterSettings::Video& v) {
        struct Candidate { const char* name; AVHWDeviceType hw; };
        const std::vector<Candidate> h264 = {{"h264_vaapi", AV_HWDEVICE_TYPE_VAAPI}, {"h264_nvenc", AV_HWDEVICE_TYPE_NONE},
                                              {"h264_vulkan", AV_HWDEVICE_TYPE_VULKAN}, {"libx264", AV_HWDEVICE_TYPE_NONE},
                                              {"libopenh264", AV_HWDEVICE_TYPE_NONE}};
        const std::vector<Candidate> hevc = {{"hevc_vaapi", AV_HWDEVICE_TYPE_VAAPI}, {"hevc_nvenc", AV_HWDEVICE_TYPE_NONE},
                                              {"hevc_vulkan", AV_HWDEVICE_TYPE_VULKAN}, {"libx265", AV_HWDEVICE_TYPE_NONE}};
        // HDR: 10-bit HEVC. Vulkan Video's 10-bit encode isn't checked for colour (its probe is 8-bit), so it's left out.
        const std::vector<Candidate> hevc10 = {{"hevc_vaapi", AV_HWDEVICE_TYPE_VAAPI}, {"hevc_nvenc", AV_HWDEVICE_TYPE_NONE},
                                                {"libx265", AV_HWDEVICE_TYPE_NONE}};
        hdr = v.hdr;
        const QByteArray only = qgetenv("EDITROOM_ENCODER");
        for (const Candidate& c : v.hdr ? hevc10 : v.hevc ? hevc : h264) {
            if (!only.isEmpty() && only != c.name) continue;
            const AVCodec* codec = avcodec_find_encoder_by_name(c.name);
            if (!codec) continue;
            AVCodecContext* ctx = avcodec_alloc_context3(codec);
            ctx->width = v.width;
            ctx->height = v.height;
            ctx->time_base = AVRational{v.fpsNum, v.fpsDen};
            ctx->framerate = AVRational{v.fpsDen, v.fpsNum};
            ctx->bit_rate = v.bitrate;
            ctx->gop_size = v.gop;
            ctx->max_b_frames = 0;
            ctx->color_primaries = v.hdr ? AVCOL_PRI_BT2020 : AVCOL_PRI_BT709;
            ctx->color_trc = v.hdr ? AVCOL_TRC_ARIB_STD_B67 : AVCOL_TRC_BT709;
            ctx->colorspace = v.hdr ? AVCOL_SPC_BT2020_NCL : AVCOL_SPC_BT709;
            ctx->color_range = AVCOL_RANGE_MPEG;
            if (v.hdr) ctx->profile = AV_PROFILE_HEVC_MAIN_10;
            if (fmt->oformat->flags & AVFMT_GLOBALHEADER) ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
            AVBufferRef* dev = nullptr;
            AVBufferRef* frames = nullptr;
            if (c.hw != AV_HWDEVICE_TYPE_NONE) {
                if (c.hw == AV_HWDEVICE_TYPE_VULKAN && !keepsColour(c.name, c.hw, v.hevc)) { avcodec_free_context(&ctx); continue; }
                if (av_hwdevice_ctx_create(&dev, c.hw, nullptr, nullptr, 0) < 0) { avcodec_free_context(&ctx); continue; }
                frames = av_hwframe_ctx_alloc(dev);
                auto* fc = reinterpret_cast<AVHWFramesContext*>(frames->data);
                fc->format = c.hw == AV_HWDEVICE_TYPE_VAAPI ? AV_PIX_FMT_VAAPI : AV_PIX_FMT_VULKAN;
                fc->sw_format = v.hdr ? AV_PIX_FMT_P010LE : AV_PIX_FMT_NV12;
                fc->width = v.width;
                fc->height = v.height;
                fc->initial_pool_size = 8;
                if (av_hwframe_ctx_init(frames) < 0) { av_buffer_unref(&frames); av_buffer_unref(&dev); avcodec_free_context(&ctx); continue; }
                ctx->pix_fmt = fc->format;
                ctx->hw_frames_ctx = av_buffer_ref(frames);
            } else {
                ctx->pix_fmt = AV_PIX_FMT_NV12;
                // libopenh264 takes only planar 4:2:0.
                if (QByteArray(c.name) == "libopenh264") ctx->pix_fmt = AV_PIX_FMT_YUV420P;
                // 10-bit: NVENC takes P010; libx265 planar 10-bit.
                if (v.hdr) ctx->pix_fmt = QByteArray(c.name) == "libx265" ? AV_PIX_FMT_YUV420P10LE : AV_PIX_FMT_P010LE;
            }
            if (!v.hevc && !v.hdr) av_opt_set(ctx->priv_data, "profile", "high", 0);
            if (v.hdr && QByteArray(c.name) != "libx265") av_opt_set(ctx->priv_data, "profile", "main10", 0);
            if (QByteArray(c.name) == "libx264") av_opt_set(ctx->priv_data, "preset", "medium", 0);
            if (QByteArray(c.name) == "libx265") av_opt_set(ctx->priv_data, "x265-params", "log-level=error", 0);
            if (avcodec_open2(ctx, codec, nullptr) < 0) {
                av_buffer_unref(&frames);
                av_buffer_unref(&dev);
                avcodec_free_context(&ctx);
                continue;
            }
            venc = ctx;
            hwDevice = dev;
            hwFrames = frames;
            encoder = QString::fromLatin1(c.name);
            vst = avformat_new_stream(fmt, nullptr);
            avcodec_parameters_from_context(vst->codecpar, ctx);
            vst->time_base = ctx->time_base;
            vst->avg_frame_rate = ctx->framerate;
            vframe = av_frame_alloc();
            vframe->format = ctx->pix_fmt == AV_PIX_FMT_YUV420P ? AV_PIX_FMT_YUV420P
                           : ctx->pix_fmt == AV_PIX_FMT_YUV420P10LE ? AV_PIX_FMT_YUV420P10LE
                           : v.hdr ? AV_PIX_FMT_P010LE : AV_PIX_FMT_NV12;
            vframe->width = v.width;
            vframe->height = v.height;
            av_frame_get_buffer(vframe, 32);
            if (hwFrames) hwframe = av_frame_alloc();
            qCInfo(lcExport) << "video encoder" << encoder;
            return true;
        }
        return false;
    }

    bool openAudio(const WriterSettings::Audio& a) {
        const AVCodec* codec = avcodec_find_encoder(a.wav ? AV_CODEC_ID_PCM_S24LE : AV_CODEC_ID_AAC);
        if (!codec) return false;
        aenc = avcodec_alloc_context3(codec);
        aenc->sample_rate = 48000;
        AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
        av_channel_layout_copy(&aenc->ch_layout, &stereo);
        aenc->sample_fmt = a.wav ? AV_SAMPLE_FMT_S32 : AV_SAMPLE_FMT_FLTP;
        aenc->time_base = AVRational{1, 48000};
        if (!a.wav) aenc->bit_rate = a.bitrate;
        if (fmt->oformat->flags & AVFMT_GLOBALHEADER) aenc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        if (avcodec_open2(aenc, codec, nullptr) < 0) return false;
        ast = avformat_new_stream(fmt, nullptr);
        avcodec_parameters_from_context(ast->codecpar, aenc);
        ast->time_base = aenc->time_base;
        aframe = av_frame_alloc();
        return true;
    }

    bool encodeAudio(int frames, const float* data) {
        aframe->nb_samples = frames;
        aframe->format = aenc->sample_fmt;
        av_channel_layout_copy(&aframe->ch_layout, &aenc->ch_layout);
        aframe->sample_rate = 48000;
        if (av_frame_get_buffer(aframe, 0) < 0) return false;
        if (aenc->sample_fmt == AV_SAMPLE_FMT_FLTP) {
            float* l = reinterpret_cast<float*>(aframe->data[0]);
            float* r = reinterpret_cast<float*>(aframe->data[1]);
            for (int i = 0; i < frames; ++i) { l[i] = data[i * 2]; r[i] = data[i * 2 + 1]; }
        } else {
            int32_t* p = reinterpret_cast<int32_t*>(aframe->data[0]);
            for (int i = 0; i < frames * 2; ++i) {
                const double v = std::clamp(double(data[i]), -1.0, 1.0);
                // 24-bit, in the top of the 32-bit sample (FFmpeg's s24le takes S32).
                p[i] = int32_t(std::lround(v * 8388607.0)) * 256;
            }
        }
        aframe->pts = audioPts;
        audioPts += frames;
        const bool ok = avcodec_send_frame(aenc, aframe) >= 0 && drain(aenc, ast);
        av_frame_unref(aframe);
        return ok;
    }
};

MediaWriter::MediaWriter() : d(std::make_unique<Impl>()) {}
MediaWriter::~MediaWriter() = default;

std::unique_ptr<MediaWriter> MediaWriter::open(const QString& path, const WriterSettings& s, QString* why) {
    prepareHardwareVideo();
    std::unique_ptr<MediaWriter> w(new MediaWriter());
    Impl& I = *w->d;
    I.path = path;
    const QByteArray p = path.toUtf8();
    const bool wav = s.audio && s.audio->wav && !s.video;
    if (avformat_alloc_output_context2(&I.fmt, nullptr, wav ? "wav" : "mp4", p.constData()) < 0 || !I.fmt) {
        if (why) *why = QStringLiteral("Couldn't create the file.");
        return nullptr;
    }
    if (s.video && !I.openVideo(*s.video)) {
        if (why) *why = QStringLiteral("This resolution isn't supported by the encoder.");
        return nullptr;
    }
    if (s.audio && !I.openAudio(*s.audio)) {
        if (why) *why = QStringLiteral("Couldn't set up the sound.");
        return nullptr;
    }
    if (!I.vst && !I.ast) {
        if (why) *why = QStringLiteral("Nothing to export.");
        return nullptr;
    }
    if (avio_open(&I.fmt->pb, p.constData(), AVIO_FLAG_WRITE) < 0) {
        if (why) *why = QStringLiteral("Couldn't create the file.");
        return nullptr;
    }
    AVDictionary* opts = nullptr;
    if (!wav) av_dict_set(&opts, "movflags", "+faststart", 0);
    const int r = avformat_write_header(I.fmt, &opts);
    av_dict_free(&opts);
    if (r < 0) {
        if (why) *why = QStringLiteral("Couldn't create the file.");
        return nullptr;
    }
    I.headerWritten = true;
    return w;
}

bool MediaWriter::writeVideo(const uint8_t* nv12, long long index) {
    Impl& I = *d;
    if (!I.venc) return false;
    if (av_frame_make_writable(I.vframe) < 0) return false;
    const int w = I.venc->width, h = I.venc->height;
    const int cw = (w + 1) / 2, ch = (h + 1) / 2;
    for (int y = 0; y < h; ++y) std::memcpy(I.vframe->data[0] + y * I.vframe->linesize[0], nv12 + size_t(y) * w, size_t(w));
    const uint8_t* uv = nv12 + size_t(w) * h;
    if (I.vframe->format == AV_PIX_FMT_YUV420P) {
        for (int y = 0; y < ch; ++y)
            for (int x = 0; x < cw; ++x) {
                I.vframe->data[1][y * I.vframe->linesize[1] + x] = uv[(size_t(y) * cw + x) * 2];
                I.vframe->data[2][y * I.vframe->linesize[2] + x] = uv[(size_t(y) * cw + x) * 2 + 1];
            }
    } else {
        for (int y = 0; y < ch; ++y) std::memcpy(I.vframe->data[1] + y * I.vframe->linesize[1], uv + size_t(y) * cw * 2, size_t(cw) * 2);
    }
    I.vframe->pts = index;
    I.vframe->color_primaries = AVCOL_PRI_BT709;
    I.vframe->color_trc = AVCOL_TRC_BT709;
    I.vframe->colorspace = AVCOL_SPC_BT709;
    I.vframe->color_range = AVCOL_RANGE_MPEG;
    AVFrame* send = I.vframe;
    if (I.hwFrames) {
        av_frame_unref(I.hwframe);
        if (av_hwframe_get_buffer(I.hwFrames, I.hwframe, 0) < 0) return false;
        if (av_hwframe_transfer_data(I.hwframe, I.vframe, 0) < 0) return false;
        av_frame_copy_props(I.hwframe, I.vframe);
        send = I.hwframe;
    }
    return avcodec_send_frame(I.venc, send) >= 0 && I.drain(I.venc, I.vst);
}

bool MediaWriter::isHDR() const { return d->hdr; }

bool MediaWriter::writeVideo10(const uint16_t* p010, long long index) {
    Impl& I = *d;
    if (!I.venc || !I.hdr) return false;
    if (av_frame_make_writable(I.vframe) < 0) return false;
    const int w = I.venc->width, h = I.venc->height;
    const int cw = (w + 1) / 2, ch = (h + 1) / 2;
    const uint16_t* uv = p010 + size_t(w) * h;
    if (I.vframe->format == AV_PIX_FMT_YUV420P10LE) {
        // Planar, the value in the bottom 10 bits.
        for (int y = 0; y < h; ++y) {
            auto* row = reinterpret_cast<uint16_t*>(I.vframe->data[0] + y * I.vframe->linesize[0]);
            for (int x = 0; x < w; ++x) row[x] = p010[size_t(y) * w + x] >> 6;
        }
        for (int y = 0; y < ch; ++y) {
            auto* u = reinterpret_cast<uint16_t*>(I.vframe->data[1] + y * I.vframe->linesize[1]);
            auto* v = reinterpret_cast<uint16_t*>(I.vframe->data[2] + y * I.vframe->linesize[2]);
            for (int x = 0; x < cw; ++x) { u[x] = uv[(size_t(y) * cw + x) * 2] >> 6; v[x] = uv[(size_t(y) * cw + x) * 2 + 1] >> 6; }
        }
    } else {
        for (int y = 0; y < h; ++y) std::memcpy(I.vframe->data[0] + y * I.vframe->linesize[0], p010 + size_t(y) * w, size_t(w) * 2);
        for (int y = 0; y < ch; ++y) std::memcpy(I.vframe->data[1] + y * I.vframe->linesize[1], uv + size_t(y) * cw * 2, size_t(cw) * 4);
    }
    I.vframe->pts = index;
    // Tagged as what they are: HLG in Rec.2020.
    I.vframe->color_primaries = AVCOL_PRI_BT2020;
    I.vframe->color_trc = AVCOL_TRC_ARIB_STD_B67;
    I.vframe->colorspace = AVCOL_SPC_BT2020_NCL;
    I.vframe->color_range = AVCOL_RANGE_MPEG;
    AVFrame* send = I.vframe;
    if (I.hwFrames) {
        av_frame_unref(I.hwframe);
        if (av_hwframe_get_buffer(I.hwFrames, I.hwframe, 0) < 0) return false;
        if (av_hwframe_transfer_data(I.hwframe, I.vframe, 0) < 0) return false;
        av_frame_copy_props(I.hwframe, I.vframe);
        send = I.hwframe;
    }
    return avcodec_send_frame(I.venc, send) >= 0 && I.drain(I.venc, I.vst);
}

bool MediaWriter::writeAudio(const float* stereo, int frames) {
    Impl& I = *d;
    if (!I.aenc) return false;
    const int fs = I.aenc->frame_size > 0 ? I.aenc->frame_size : 4096;
    I.pendingAudio.insert(I.pendingAudio.end(), stereo, stereo + size_t(frames) * 2);
    size_t at = 0;
    while (I.pendingAudio.size() - at >= size_t(fs) * 2) {
        if (!I.encodeAudio(fs, I.pendingAudio.data() + at)) return false;
        at += size_t(fs) * 2;
    }
    I.pendingAudio.erase(I.pendingAudio.begin(), I.pendingAudio.begin() + qsizetype(at));
    return true;
}

bool MediaWriter::finish() {
    Impl& I = *d;
    bool ok = true;
    if (I.aenc) {
        if (!I.pendingAudio.empty()) {
            const int left = int(I.pendingAudio.size() / 2);
            const int fs = I.aenc->frame_size > 0 && !(I.aenc->codec->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE) ? I.aenc->frame_size : left;
            std::vector<float> last(size_t(std::max(fs, left)) * 2, 0.0f);
            std::copy(I.pendingAudio.begin(), I.pendingAudio.end(), last.begin());
            ok = I.encodeAudio(std::max(fs, left), last.data()) && ok;
            I.pendingAudio.clear();
        }
        avcodec_send_frame(I.aenc, nullptr);
        ok = I.drain(I.aenc, I.ast) && ok;
    }
    if (I.venc) {
        avcodec_send_frame(I.venc, nullptr);
        ok = I.drain(I.venc, I.vst) && ok;
    }
    ok = av_write_trailer(I.fmt) >= 0 && ok;
    return ok;
}

void MediaWriter::abort() {
    if (d->fmt && d->fmt->pb) avio_closep(&d->fmt->pb);
    QFile::remove(d->path);
}

QString MediaWriter::encoderName() const { return d->encoder.isEmpty() ? (d->aenc ? QString::fromLatin1(d->aenc->codec->name) : QString()) : d->encoder; }

} // namespace er
