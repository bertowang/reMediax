package com.tencent.tgcapture;

/**
 * 视频叠加层描述（对应 C++ OverlayDesc）。画在主画面之上、水印之下；
 * 位置 / 大小是画布宽高的比例，运行期切分辨率后自动跟随。
 */
public final class OverlayDesc {
    /** 静态图片（png，带透明通道按透明度混合） */
    public static final int KIND_IMAGE = 0;
    /** 本地视频文件（mp4 / mov / flv 里的 H.264），按实时节奏播放 */
    public static final int KIND_VIDEO_FILE = 1;

    /** 唯一标识（字母数字 . _ -），必填 */
    public String id = "";
    public int kind = KIND_IMAGE;
    /** 本地文件绝对路径（App 需有读权限，例如 getExternalFilesDir 下） */
    public String path = "";
    /** 视频：播完从头再来；false = 停在最后一帧并回调 onNotify(NOTIFY_LOCAL_FILE_EOF, "overlay:" + id) */
    public boolean loop = true;
    /** 左上角，画布比例 -1 ~ 1 */
    public float x = 0f;
    public float y = 0f;
    /** 宽，画布宽的比例 (0, 2] */
    public float width = 0.25f;
    /** 高，画布高的比例 [0, 2]；0 = 按素材宽高比 */
    public float height = 0f;
    /** 不透明度 0 ~ 1 */
    public float opacity = 1f;
    /** 越大越靠上；相同按 id 排序 */
    public int zOrder = 0;
    public boolean visible = true;

    public OverlayDesc() {
    }

    public OverlayDesc(String id, int kind, String path) {
        this.id = id;
        this.kind = kind;
        this.path = path;
    }

    public OverlayDesc copy() {
        OverlayDesc d = new OverlayDesc(id, kind, path);
        d.loop = loop;
        d.x = x;
        d.y = y;
        d.width = width;
        d.height = height;
        d.opacity = opacity;
        d.zOrder = zOrder;
        d.visible = visible;
        return d;
    }

    @Override
    public String toString() {
        return String.format(java.util.Locale.US, "%s[%s %s x=%.2f y=%.2f w=%.2f h=%.2f a=%.2f z=%d%s%s]", id,
                kind == KIND_VIDEO_FILE ? "video" : "image", path, x, y, width, height, opacity, zOrder,
                loop ? "" : " noloop", visible ? "" : " hidden");
    }
}
