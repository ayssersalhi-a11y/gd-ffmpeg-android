/**
 * ffmpeg_player.h
 * GDExtension - FFmpeg Video Player (Video Only) for Godot 4 (Android ARM64/ARM32)
 *
 * الإصدار 3.0 — فصل كامل للصوت عن الفيديو + إصلاح التقطع
 *
 * المبدأ الجديد:
 *   - FFmpegPlayer يعرض الفيديو فقط (بدون أي معالجة صوتية داخلية).
 *   - الصوت يُحمَّل بشكل منفصل عبر load_audio(path) من ملف mp3/ogg
 *     محلي (res:// أو مسار مطلق) أو رابط HTTP/HTTPS مُخزَّن مسبقاً.
 *   - المزامنة تعتمد على ساعة زمنية داخلية (delta accumulator) وليس
 *     على AudioStreamPlayer، مما يُزيل كل أسباب desync نهائياً.
 *   - إصلاح التقطع: decoded_frame_queue يُفكّك إطارات مسبقاً بعيداً عن
 *     دورة _process، فيصبح عرض كل إطار < 1ms.
 */

#pragma once

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/audio_stream_player.hpp>
#include <godot_cpp/classes/audio_stream_mp3.hpp>
#include <godot_cpp/classes/audio_stream_ogg_vorbis.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/core/class_db.hpp>

#include <list>
#include <vector>
#include <deque>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libavutil/time.h>
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
}

namespace godot {

// بنية تحمل إطار فيديو مُفكَّكاً ومُحوَّلاً إلى RGB24، جاهز للعرض فوراً
struct DecodedFrame {
    PackedByteArray data;   // بيانات RGB24 جاهزة للرفع إلى GPU
    double          pts;    // وقت العرض بالثواني
};

class FFmpegPlayer : public Node {
    GDCLASS(FFmpegPlayer, Node)

public:
    FFmpegPlayer();
    ~FFmpegPlayer();

    // ─── Video API ────────────────────────────────────────────────────────────
    bool load_video(const String &path);
    void play();
    void pause();
    void stop();
    void seek(double seconds);

    bool   is_playing()              const;
    double get_duration()            const;
    double get_position()            const;
    int    get_video_width()         const;
    int    get_video_height()        const;
    double get_fps()                 const;
    Ref<ImageTexture> get_current_frame_texture() const;

    void  set_loop(bool enable);
    bool  get_loop() const;

    // ─── External Audio API ───────────────────────────────────────────────────
    //
    // اختر لغة الصوت (عربي أو إنجليزي أو أي ملف):
    //   player.load_audio("res://audio/arabic.mp3")
    //   player.load_audio("res://audio/english.mp3")
    //   player.load_audio("/storage/emulated/0/audio/custom.ogg")
    //
    // ثم شغّل الفيديو كالمعتاد: player.play()
    // الصوت يبدأ تلقائياً مع الفيديو ويُوقَف/يُكمَل معه.
    //
    bool  load_audio(const String &path);   // يُحمَّل ملف صوتي منفصل
    void  unload_audio();                   // إزالة الصوت الحالي

    void  set_audio_volume(float vol);      // 0.0 (صامت) إلى 1.0 (أقصى)
    float get_audio_volume()       const;
    void  set_audio_muted(bool muted);      // كتم الصوت دون تغيير مستواه
    bool  is_audio_muted()         const;
    String get_loaded_audio_path() const;   // اسم الملف المحمَّل حالياً

    // ─── Buffer Info (للـ UI) ─────────────────────────────────────────────────
    double get_forward_buffer()  const { return forward_buffer_secs; }
    bool   is_buffering()        const { return buffering; }

    // ─── Godot Overrides ─────────────────────────────────────────────────────
    void _ready()               override;
    void _process(double delta) override;

protected:
    static void _bind_methods();

private:
    // ─── FFmpeg: سياق الملف والفيديو فقط (لا صوت) ───────────────────────────
    AVFormatContext *fmt_ctx          = nullptr;
    AVCodecContext  *video_codec_ctx  = nullptr;
    SwsContext      *sws_ctx          = nullptr;
    int              video_stream_idx = -1;

    int    video_width  = 0;
    int    video_height = 0;
    double fps          = 0.0;
    double duration     = 0.0;

    uint8_t          *frame_buffer   = nullptr;
    Ref<ImageTexture> current_texture;

    // ─── طابور الحزم الخام (قبل فك التشفير) ─────────────────────────────────
    std::list<AVPacket*> video_packet_queue;

    // ─── طابور الإطارات المُفكَّكة (الحل الجوهري لمشكلة التقطع) ─────────────
    // _decode_packets_into_queue() تملأ هذا الطابور في الخلفية.
    // _present_frame_at() تأخذ الإطار الصحيح منه بسرعة < 1ms.
    std::deque<DecodedFrame> decoded_frame_queue;
    static const int MAX_DECODED_FRAMES = 8;

    // ─── حالة التشغيل ────────────────────────────────────────────────────────
    bool   playing           = false;
    bool   looping           = false;
    bool   buffering         = false;
    bool   is_streaming      = false;
    double position          = 0.0;
    double stream_start_time = 0.0;
    double frame_timer       = 0.0;

    // ─── نظام البافر ─────────────────────────────────────────────────────────
    double forward_buffer_secs = 0.0;
    const double MAX_FORWARD   = 40.0;
    const double MIN_FORWARD   = 20.0;
    const double INITIAL_PLAY  = 5.0;

    // ─── الصوت الخارجي ───────────────────────────────────────────────────────
    AudioStreamPlayer *ext_audio_player = nullptr;
    String             loaded_audio_path;
    float              audio_volume      = 1.0f;
    bool               audio_muted       = false;

    // ─── الدوال الداخلية ─────────────────────────────────────────────────────
    void _read_packets_to_queue();
    void _prefill_buffers();
    void _update_buffer_stats();
    int  _calc_read_batch_size() const;

    void _decode_packets_into_queue();
    bool _present_frame_at(double current_pos);
    void _apply_audio_volume();

    void _allocate_buffers();
    void _clear_queues();
    void _cleanup();

    void _emit_video_loaded(bool success);
    void _emit_video_finished();
    void _emit_frame_updated();
    void _emit_playback_error(const String &message);
};

} // namespace godot
