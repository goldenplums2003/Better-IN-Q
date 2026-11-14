#pragma once
// ============================================================================
//  foodgate.h —— 什么时候把配置里的猫饭写进游戏
//
//  猫饭是在读盘（加载画面）期间写进去的，进任务时游戏按它给饭的效果。老 IN-Q 每逢读盘都写，
//  所以用过一次之后，自己吃了饭、正常接任务出发，也会被换成配置里的猫饭（或者写到一半效果没了）。
//  现在只在插件自己重开的那一轮写：发出重开请求时「上膛」，重开流程走完（状态机回到 0）之后
//  的那一次读盘结束就「退膛」。重开失败也退膛。always = 设置里勾了「正常进任务也吃」，每次读盘都写。
// ============================================================================

#include <algorithm>

// ============================================================================
//  「当前猫饭」：记住玩家自己吃的猫饭
//
//  游戏里生效的猫饭和预设是同一块 8 个数（体力、耐力、攻击、防御、属性耐性、技能×3）。
//  重开后游戏不再给饭的效果，所以平时（不在读盘时）每拍看一眼，有饭就记下；
//  选了「当前猫饭」的话，重开进任务的那次读盘把记下的写回去。
//  没饭（全 0、技能全是 -1 或全是 0）和不像猫饭的数（越界）不记：被游戏清掉之后，记着的还是上一份。
// ============================================================================
struct MealMemory {
    bool has = false;
    int v[8] = {0, 0, 0, 0, 0, -1, -1, -1};

    // 范围同配置里的预设：体力 0~5，耐力 0~2，攻击 / 防御 / 耐性 0~3，技能 -1~56
    static bool Valid(const int* m)
    {
        static const int hi[5] = {5, 2, 3, 3, 3};
        for (int i = 0; i < 5; ++i) if (m[i] < 0 || m[i] > hi[i]) return false;
        for (int i = 5; i < 8; ++i) if (m[i] < -1 || m[i] > 56) return false;
        return true;
    }
    static bool Empty(const int* m)
    {
        for (int i = 0; i < 5; ++i) if (m[i] != 0) return false;
        const bool none = m[5] == -1 && m[6] == -1 && m[7] == -1;
        const bool zero = m[5] == 0 && m[6] == 0 && m[7] == 0;   // 整块清成 0 的情况（三个技能都是 0 号的饭不存在）
        return none || zero;
    }
    // 每拍把游戏里现在的 8 个数给它。返回 true = 记下了新的一份
    bool Observe(const int* m, bool loading)
    {
        if (loading || !Valid(m) || Empty(m)) return false;
        if (has && std::equal(m, m + 8, v)) return false;
        std::copy(m, m + 8, v);
        has = true;
        return true;
    }
};

struct FoodGate {
    bool armed = false;       // 这一轮重开还没进完任务
    bool finalLoad = false;   // 重开流程已经走完，正在进任务的那次读盘

    void Arm() { armed = true; finalLoad = false; }
    void Disarm() { armed = false; finalLoad = false; }

    // 每 100ms 调一次。loading = 正在读盘，restart = 重开状态机（0 = 空闲）。返回这一拍要不要写猫饭
    bool Step(bool loading, int restart, bool always)
    {
        if (armed && restart == 0 && loading) finalLoad = true;
        const bool write = loading && (always || armed);
        if (armed && finalLoad && !loading) Disarm();   // 进任务那次读盘结束
        return write;
    }
};
