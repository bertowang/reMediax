// 极简单测框架：零第三方依赖（仓库本身不引入 gtest），够用即可。
//
//   TG_TEST(Suite, Name) { TG_EXPECT(cond); TG_EXPECT_EQ(a, b); TG_ASSERT(cond); }
//
//   - TG_EXPECT*：失败记录后继续
//   - TG_ASSERT*：失败立即结束当前用例
//   运行：./tg_tests [过滤子串]    例：./tg_tests Bus
#ifndef TGCAPTURE_TESTS_TG_TEST_H
#define TGCAPTURE_TESTS_TG_TEST_H

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>
#include <string>
#include <type_traits>
#include <vector>

namespace tgtest
{

struct Case
{
    std::string name;
    std::function<void()> fn;
};

inline std::vector<Case>& Registry()
{
    static std::vector<Case> cases;
    return cases;
}

struct Registrar
{
    Registrar(const char* suite, const char* name, std::function<void()> fn)
    {
        Registry().push_back({std::string(suite) + "." + name, std::move(fn)});
    }
};

inline int& FailCount()
{
    static int n = 0;
    return n;
}

struct AssertAbort {};

inline void Report(const char* file, int line, const std::string& msg)
{
    ++FailCount();
    std::printf("    FAIL %s:%d  %s\n", file, line, msg.c_str());
}

template <typename A, typename B>
std::string Describe(const char* ea, const char* eb, const A& a, const B& b)
{
    std::string out = std::string(ea) + " == " + eb + "  (";
    if constexpr (std::is_arithmetic_v<A> && std::is_arithmetic_v<B>) {
        out += std::to_string(a) + " vs " + std::to_string(b);
    } else {
        out += "值不可打印";
    }
    return out + ")";
}

inline int RunAll(int argc, char** argv)
{
    const std::string filter = argc > 1 ? argv[1] : "";
    int ran = 0;
    int failed = 0;
    for (const auto& c : Registry()) {
        if (!filter.empty() && c.name.find(filter) == std::string::npos) {
            continue;
        }
        ++ran;
        const int before = FailCount();
        std::printf("[ RUN  ] %s\n", c.name.c_str());
        std::fflush(stdout);
        try {
            c.fn();
        } catch (const AssertAbort&) {
        } catch (const std::exception& e) {
            Report(__FILE__, __LINE__, std::string("未捕获异常: ") + e.what());
        }
        if (FailCount() != before) {
            ++failed;
            std::printf("[ FAIL ] %s\n", c.name.c_str());
        } else {
            std::printf("[  OK  ] %s\n", c.name.c_str());
        }
    }
    std::printf("\n%d 个用例，%d 个失败\n", ran, failed);
    return failed == 0 ? 0 : 1;
}

}  // namespace tgtest

#define TG_TEST(suite, name)                                                         \
    static void tgtest_##suite##_##name();                                           \
    static ::tgtest::Registrar tgtest_reg_##suite##_##name(#suite, #name,            \
                                                           tgtest_##suite##_##name); \
    static void tgtest_##suite##_##name()

#define TG_EXPECT(cond)                                                              \
    do {                                                                             \
        if (!(cond)) ::tgtest::Report(__FILE__, __LINE__, #cond);                    \
    } while (0)

#define TG_EXPECT_EQ(a, b)                                                           \
    do {                                                                             \
        const auto tg_a = (a);   /* 按值：a 可能是 *optional 临时对象，绑引用会悬空 */ \
        const auto tg_b = (b);                                                       \
        if (!(tg_a == tg_b))                                                         \
            ::tgtest::Report(__FILE__, __LINE__, ::tgtest::Describe(#a, #b, tg_a, tg_b)); \
    } while (0)

#define TG_ASSERT(cond)                                                              \
    do {                                                                             \
        if (!(cond)) {                                                               \
            ::tgtest::Report(__FILE__, __LINE__, #cond);                             \
            throw ::tgtest::AssertAbort{};                                           \
        }                                                                            \
    } while (0)

#endif  // TGCAPTURE_TESTS_TG_TEST_H
