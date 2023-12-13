// Fill out your copyright notice in the Description page of Project Settings.


#include "Player/FFmpegMediaPlayer.h"
#include "Async/Async.h"
#include "FFmpegMediaTracks.h"
#include "IMediaEventSink.h"
#include "FFmpegMediaSettings.h"

extern  "C" {
#include "libavformat/avformat.h"
#include "libavutil/opt.h"
#include "libavutil/error.h"
#include "libavutil/time.h"
}

#define FF_INPUT_BUFFER_PADDING_SIZE 32

FFmpegMediaPlayer::FFmpegMediaPlayer(IMediaEventSink& InEventSink)
    : EventSink(InEventSink)
    , Tracks(MakeShared<FFFmpegMediaTracks, ESPMode::ThreadSafe>())
{
    this->abort_request = 0;
    this->ic = nullptr;
    this->IOContext = nullptr;
}

FFmpegMediaPlayer::~FFmpegMediaPlayer()
{
}

/**
 * @brief 初始化播放器
 * @param Archive 附件
 * @param Url 播放地址
 * @param Precache 是否预缓存
 * @param PlayerOptions 播放器选项
 * @return 
*/
bool FFmpegMediaPlayer::InitializePlayer(const TSharedPtr<FArchive, ESPMode::ThreadSafe>& Archive, const FString& Url, bool Precache, const FMediaPlayerOptions* PlayerOptions)
{
    UE_LOG(LogFFmpegMedia, Verbose, TEXT("Player %p: Initializing %s (archive = %s, precache = %s)"), this, *Url, Archive.IsValid() ? TEXT("yes") : TEXT("no"), Precache ? TEXT("yes") : TEXT("no"));

    this->MediaUrl = Url; //设置当前播放地址

    const EAsyncExecution Execution = Precache ? EAsyncExecution::Thread : EAsyncExecution::ThreadPool; //创建异步执行器

    Async(Execution, [Archive, Url, Precache, PlayerOptions, TracksPtr = TWeakPtr<FFFmpegMediaTracks, ESPMode::ThreadSafe>(Tracks), ThisPtr = this]()
        {
            //轨道对象Tracks的弱引用
            TSharedPtr<FFFmpegMediaTracks, ESPMode::ThreadSafe> PinnedTracks = TracksPtr.Pin();

            if (PinnedTracks.IsValid())
            {
                //读取媒体信息，获取AVFormatContext 
                AVFormatContext* context = ThisPtr->ReadContext(Archive, Url, Precache);
                if (context) {
                    //通过AVFormatContext初始化轨道对象
                    PinnedTracks->Initialize(context, Url, PlayerOptions);
                }
            }
        });
    return true;
}

/** [Custom] 读取Content */
AVFormatContext* FFmpegMediaPlayer::ReadContext(const TSharedPtr<FArchive, ESPMode::ThreadSafe>& Archive, const FString& Url, bool Precache)
{
    //取消中断
    this->abort_request = 0;

    int err,ret;
    int scan_all_pmts_set = 0;
    const AVDictionaryEntry* t;
    AVFormatContext* ic_ = nullptr;
    AVDictionary* format_opts = nullptr;
    ic_ = avformat_alloc_context(); //分配上下文
    int genpts = 0; //生成pts, 默认为0 todo: 放入setting中

    if (!ic_) {
        UE_LOG(LogFFmpegMedia, Error, TEXT("Player %p: Could not allocate context"), this);
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    ic_->interrupt_callback.callback = decode_interrupt_cb;// 设置超时回调
    ic_->interrupt_callback.opaque = this;
    if (!av_dict_get(format_opts, "scan_all_pmts", NULL, AV_DICT_MATCH_CASE)) {
        av_dict_set(&format_opts, "scan_all_pmts", "1", AV_DICT_DONT_OVERWRITE);
        scan_all_pmts_set = 1;
    }

    if (!Archive.IsValid()) //如果附件对象不可用，则获取Url
    {
        if (Url.StartsWith(TEXT("file://")))//如果是文件开头
        {
            const char* fileName = TCHAR_TO_UTF8(&Url[7]);
            AVDictionary* opts = NULL;
            err = avformat_open_input(&ic_, fileName, NULL, &format_opts);
        }
        else {
            err = avformat_open_input(&ic_, TCHAR_TO_UTF8(*Url), NULL, &format_opts);
        }
    }
    else //如果是附件，则转化成内存文件
    { 
        UE_LOG(LogFFmpegMedia, Error, TEXT("Player %p: Unsupport Archive..."), this);
        return NULL;
        CurrentArchive = Archive;
        const int ioBufferSize = 32768;
        unsigned char* ioBuffer = (unsigned char*)av_malloc(ioBufferSize + FF_INPUT_BUFFER_PADDING_SIZE);
        IOContext = avio_alloc_context(ioBuffer, ioBufferSize, 0, this, ReadtStreamCallback, NULL, SeekStreamCallback);
        ic_->pb = IOContext;
        err = avformat_open_input(&ic_, "InMemoryFile", NULL, &format_opts);
    }

    if (err < 0) {
        char errbuf[1024] = {};
        av_strerror(err, errbuf, 1024);
        UE_LOG(LogFFmpegMedia, Error, TEXT("Player %p: Couldn't Open File %d(%s)"), this, err, errbuf);
        ret = -1;
        goto fail;
    }

    if (scan_all_pmts_set)
        av_dict_set(&format_opts, "scan_all_pmts", NULL, AV_DICT_MATCH_CASE);

    t = av_dict_get(format_opts, "", NULL, AV_DICT_IGNORE_SUFFIX);
    if (t) {
        UE_LOG(LogFFmpegMedia, Error, TEXT("Player %p: format_opts %s not found."), this, t->key);
        ret = AVERROR_OPTION_NOT_FOUND;
        goto fail;
    }

    this->ic = ic_;
    if (genpts)
        this->ic->flags |= AVFMT_FLAG_GENPTS;

    err = avformat_find_stream_info(ic, nullptr);
    if (err < 0) {
        UE_LOG(LogFFmpegMedia, Error, TEXT("Player %p: could not find codec parameters."), this);
        ret = -1;
        goto fail;
    }

    //const auto Settings = GetDefault<UFFmpegMediaSettings>();
    if (ic->pb)
        ic->pb->eof_reached = 0; // FIXME hack, ffplay maybe should not use avio_feof() to test for the end
    
    return ic;
fail:
    if (!ic) {
        avformat_close_input(&ic);
    }
    this->abort_request = 1;
    if (ret != 0) {
        //发送媒体打开失败事件
        EventSink.ReceiveMediaEvent(EMediaEvent::MediaOpenFailed);
    }
    return nullptr;
}

int FFmpegMediaPlayer::decode_interrupt_cb(void* ctx)
{
    FFmpegMediaPlayer* player = static_cast<FFmpegMediaPlayer*>(ctx);
    //注意abort_request为true时，会直接返回1，导致立即取消阻塞状态，所以阻塞进程执行之前，比如avformat_open_input，一定要为false, 否则还没读取之前就会直接中断
    return player->abort_request ? 1 : 0;
}

/** [Custom] 读取流回调 */
int FFmpegMediaPlayer::ReadtStreamCallback(void* opaque, uint8_t* buf, int buf_size) {
    FFmpegMediaPlayer* player = static_cast<FFmpegMediaPlayer*>(opaque);
    int64 Position = player->CurrentArchive->Tell();
    int64 Size = player->CurrentArchive->TotalSize();
    int64 BytesToRead = buf_size;
    if (BytesToRead > (int64)Size)
    {
        BytesToRead = Size;
    }

    if ((Size - BytesToRead) < player->CurrentArchive->Tell())
    {
        BytesToRead = Size - Position;
    }
    if (BytesToRead > 0)
    {
        player->CurrentArchive->Serialize(buf, BytesToRead);
    }

    player->CurrentArchive->Seek(Position + BytesToRead);

    return BytesToRead;
}

/** [Custom] 跳转流回调 */
int64_t FFmpegMediaPlayer::SeekStreamCallback(void* opaque, int64_t offset, int whence) {
    FFmpegMediaPlayer* player = static_cast<FFmpegMediaPlayer*>(opaque);
    if (whence == AVSEEK_SIZE) {
        return player->CurrentArchive->TotalSize();
    }
    int64_t pos = player->CurrentArchive->Tell();
    player->CurrentArchive->Seek(pos + offset);
    return player->CurrentArchive->Tell();

}

/* IMediaPlayer 接口实现 开始*/
/* ***************************************************************************** */
bool FFmpegMediaPlayer::Open(const FString& Url, const IMediaOptions* Options)
{
    return Open(Url, Options, nullptr);
}

bool FFmpegMediaPlayer::Open(const FString& Url, const IMediaOptions* Options, const FMediaPlayerOptions* PlayerOptions)
{
    Close(); //关闭旧媒体

    if (Url.IsEmpty())
    {
        UE_LOG(LogFFmpegMedia, Error, TEXT("Player %p:Cannot open media from url(url is empty)"), this);
        return false;
    }

    UE_LOG(LogFFmpegMedia, Log, TEXT("Player %p: Open Media Source[Url]: [%s]"), this, *Url);

    const bool Precache = (Options != nullptr) ? Options->GetMediaOption("PrecacheFile", false) : false;
    return InitializePlayer(nullptr, Url, Precache, nullptr);
}

/**
 * @brief 根据文件或内存归档和可选参数打开媒体源
 * @param Archive 
 * @param OriginalUrl 
 * @param Options 
 * @return 
*/
bool FFmpegMediaPlayer::Open(const TSharedRef<FArchive, ESPMode::ThreadSafe>& Archive, const FString& OriginalUrl, const IMediaOptions* Options)
{
    Close(); //关闭旧媒体

    if (Archive->TotalSize() == 0)
    {
        UE_LOG(LogFFmpegMedia, Error, TEXT("Player %p: Cannot open media from archive (archive is empty)"), this);
        return false;
    }

    if (OriginalUrl.IsEmpty())
    {
        UE_LOG(LogFFmpegMedia, Error, TEXT("Player %p: Cannot open media from archive (no original URL provided)"), this);
        return false;
    }

    UE_LOG(LogFFmpegMedia, Log, TEXT("Player %p: Open Media Source[Archive]: %s"), this);
    return InitializePlayer(Archive, OriginalUrl, false, nullptr);
}

void FFmpegMediaPlayer::Close()
{
    UE_LOG(LogFFmpegMedia, Log, TEXT("FFmpegMediaPlayer %p: Close ...."), this);
    if (Tracks->GetState() == EMediaState::Closed) //如果轨道对象状态为关闭，直接返回
    {
        return;
    }
    
    //中断
    this->abort_request = 1;

    this->MediaUrl = FString(); //重置MeidaUrl
    this->Tracks->Shutdown();  //关闭轨道

    //清除ffmpeg资源占用
    if (this->ic) {
        //todo: 已经在Tracks中关闭了，二次关闭会报错
        //avformat_close_input(&IC);
        this->ic = nullptr;
    }

    //清除FFmpeg IOContext(读取内存文件时才会存在)
    if (IOContext) {
        av_free(IOContext->buffer);
        av_free(IOContext);
        IOContext = nullptr;
    }

    //通知监听器
    EventSink.ReceiveMediaEvent(EMediaEvent::TracksChanged);
    EventSink.ReceiveMediaEvent(EMediaEvent::MediaClosed); 
}

void FFmpegMediaPlayer::TickInput(FTimespan DeltaTime, FTimespan Timecode)
{

    bool MediaSourceChanged = false; //数据源是否变动
    bool TrackSelectionChanged = false; //轨道选择是否变动

    Tracks->GetFlags(MediaSourceChanged, TrackSelectionChanged);

    if (MediaSourceChanged)
    {
        EventSink.ReceiveMediaEvent(EMediaEvent::TracksChanged); //发送轨道改变事件，注意如果不发送改事件，无法触发FMediaPlayerFacade::SelectDefaultTracks方法调用
    }
    else {
        if (TrackSelectionChanged)
        {
            EventSink.ReceiveMediaEvent(EMediaEvent::TracksChanged);
        }
    }

    //如果修改过，则发送事件之后重置状态为false
    if (MediaSourceChanged || TrackSelectionChanged)
    {
        Tracks->ClearFlags();
    }

    //将Tracks对象中的事件取出循环执行
    TArray<EMediaEvent> OutEvents;
    Tracks->GetEvents(OutEvents);
    for (const auto& Event : OutEvents)
    {
        EventSink.ReceiveMediaEvent(Event);
    }
}


bool FFmpegMediaPlayer::FlushOnSeekStarted() const
{
    return false; //Seek开始时清除样本，当前seek操作是基于异步完成的，所以seek开始前不需要请求样本，让其继续播放
}

bool FFmpegMediaPlayer::FlushOnSeekCompleted() const
{
    return false; //Seek完成时清除样本
    //return this->Tracks->IsOnlyHasVideo();
}

bool FFmpegMediaPlayer::GetPlayerFeatureFlag(EFeatureFlag flag) const
{
    switch (flag)
    {
        case EFeatureFlag::UsePlaybackTimingV2: //音视频同步机制
        case EFeatureFlag::PlayerUsesInternalFlushOnSeek: //阻止EMediaEvent::SeekCompleted时的Flush
            return true;
        case EFeatureFlag::AlwaysPullNewestVideoFrame: //处理只有视频流时，播放卡主问题, 当只有视频流时，默认获取最新的帧进行播放
            return this->Tracks->IsOnlyHasVideo();
    }
    return IMediaPlayer::GetPlayerFeatureFlag(flag);
}

IMediaCache& FFmpegMediaPlayer::GetCache()
{
	return *this;
}

IMediaControls& FFmpegMediaPlayer::GetControls()
{
    return *Tracks;
}

FString FFmpegMediaPlayer::GetInfo() const
{
    return Tracks->GetMeidaInfo();
}

FGuid FFmpegMediaPlayer::GetPlayerPluginGUID() const
{
    //注意此处要与FFmpegMediaFactory中的一致
    static FGuid PlayerPluginGUID(0x688ae1e8, 0x9b647f80, 0x9ce98ced, 0x9daa4ca6);
    return PlayerPluginGUID;
}

IMediaSamples& FFmpegMediaPlayer::GetSamples()
{
    return Tracks->GetSamples();
}

FString FFmpegMediaPlayer::GetStats() const
{
    //todo:
    UE_LOG(LogFFmpegMedia, Log, TEXT("FFmpegMediaPlayer %p: GetStats ...."), this);
	return FString("todo...");
}

IMediaTracks& FFmpegMediaPlayer::GetTracks()
{
    return *Tracks;
}

FString FFmpegMediaPlayer::GetUrl() const
{
    return MediaUrl;
}

IMediaView& FFmpegMediaPlayer::GetView()
{
    return *this;
}







