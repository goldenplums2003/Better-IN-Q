#pragma once
// ============================================================================
//  foodgate.h —— 什么时候把配置里的猫饭写进游戏
//
//  猫饭是在读盘（加载画面）期间写进去的，进任务时游戏按它给饭的效果。老 IN-Q 每逢读盘都写，
//  所以用过一次之后，自己吃了饭、正常接任务出发，也会被换成配置里的猫饭（或者写到一半效果没了）。
//  现在只在插件自己重开的那一轮写：发出重开请求时「上膛」，重开流程走完（状态机回到 0）之后
//  的那一次读盘结束就「退膛」。重开失败也退膛。always = 设置里勾了「正常进任务也吃」，每次读盘都写。
// ============================================================================

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
