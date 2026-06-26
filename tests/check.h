#pragma once
#include <cstdio>
#include <cstring>

static int g_checks = 0;
static int g_fails  = 0;

#define CHECK(cond) do { ++g_checks; if(!(cond)) { ++g_fails; \
    std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while(0)

#define CHECK_EQ(a,b) do { ++g_checks; if(!((a)==(b))) { ++g_fails; \
    std::printf("  FAIL %s:%d: %s == %s\n", __FILE__, __LINE__, #a, #b); } } while(0)

#define CHECK_STR(a,b) do { ++g_checks; if(std::strcmp((a),(b))!=0) { ++g_fails; \
    std::printf("  FAIL %s:%d: \"%s\" == \"%s\"\n", __FILE__, __LINE__, (a), (b)); } } while(0)

#define RUN(fn) do { std::printf("-- %s\n", #fn); fn(); } while(0)

#define REPORT() do { std::printf("\n%d checks, %d failures\n", g_checks, g_fails); \
    return g_fails ? 1 : 0; } while(0)
