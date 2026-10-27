#include "core/UrlRedact.h"
#include "tg_test.h"

using TGCapture::core::RedactUrl;

TG_TEST(UrlRedact, RtmpKeepsHostAndAppMasksStreamKey)
{
    TG_EXPECT_EQ(RedactUrl("rtmp://push.example.com/live/abc123secret"),
                 std::string("rtmp://push.example.com/live/***"));
}

TG_TEST(UrlRedact, QueryIsMaskedEntirely)
{
    TG_EXPECT_EQ(RedactUrl("rtmp://h:1935/live/key?txSecret=aaa&txTime=bbb"),
                 std::string("rtmp://h:1935/live/***?***"));
    TG_EXPECT_EQ(RedactUrl("srt://h:9000?streamid=#!::r=live/x,m=publish"),
                 std::string("srt://h:9000?***"));
}

TG_TEST(UrlRedact, UserInfoIsMasked)
{
    TG_EXPECT_EQ(RedactUrl("rtmp://user:pass@h/live/key"), std::string("rtmp://***@h/live/***"));
}

TG_TEST(UrlRedact, SingleSegmentPathIsMasked)
{
    TG_EXPECT_EQ(RedactUrl("rtmp://h/onlykey"), std::string("rtmp://h/***"));
}

TG_TEST(UrlRedact, TrailingSlashHasNothingToMask)
{
    TG_EXPECT_EQ(RedactUrl("rtmp://h/live/"), std::string("rtmp://h/live/"));
}

TG_TEST(UrlRedact, NoPathNoQuery)
{
    TG_EXPECT_EQ(RedactUrl("srt://h:9000"), std::string("srt://h:9000"));
}

TG_TEST(UrlRedact, LocalPathsAreUntouched)
{
    TG_EXPECT_EQ(RedactUrl("/tmp/out.flv"), std::string("/tmp/out.flv"));
    TG_EXPECT_EQ(RedactUrl("out.flv"), std::string("out.flv"));
    TG_EXPECT_EQ(RedactUrl(""), std::string(""));
}

TG_TEST(UrlRedact, SecretNeverSurvives)
{
    const char* urls[] = {
        "rtmp://h/live/TOPSECRET",
        "rtmp://h/live/x?token=TOPSECRET",
        "srt://h:1?streamid=TOPSECRET",
        "rtmp://u:TOPSECRET@h/live/x",
    };
    for (const char* u : urls) {
        TG_EXPECT(RedactUrl(u).find("TOPSECRET") == std::string::npos);
    }
}
