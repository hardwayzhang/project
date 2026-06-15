/*
 * 手动激活 / 反激活 AddressSanitizer 检查的封装。
 *
 * 背景：
 *   ASan 支持 ASAN_OPTIONS=start_deactivated=1，让运行时以“未激活”状态启动
 *   （不毒化堆、不做越界检查），之后再激活。该机制最初是为 Android 设计：
 *   主程序不带插桩，等到 dlopen 一个带插桩的 .so 时才自动激活。
 *
 *   激活/反激活由运行时内部函数 __asan::AsanActivate() / AsanDeactivate()
 *   完成。它们 **不是公开稳定 API**，没有官方头文件，但确实存在于 ASan
 *   运行时里，可供学习/演示时调用。
 *
 * 使用前提（重要）：
 *   1) 必须 **静态链接** ASan 运行时，符号才可被引用：
 *        - gcc ：加 -static-libasan
 *        - clang：默认即静态（如需共享才用 -shared-libsan）
 *      若用 gcc 默认的共享 libasan.so，这两个符号不导出，链接会失败。
 *   2) 这是内部实现细节，不同 LLVM/GCC 版本可能变化，勿用于生产代码。
 */
#ifndef ASAN_ACTIVATION_H
#define ASAN_ACTIVATION_H

#ifdef __cplusplus
/* C++：直接按命名空间声明内部函数。 */
namespace __asan {
void AsanActivate();
void AsanDeactivate();
}
static inline void asan_activate(void)   { __asan::AsanActivate(); }
static inline void asan_deactivate(void) { __asan::AsanDeactivate(); }
#else
/* C：用 C++ 修饰后的符号名（mangled name）声明。
 *   __asan::AsanActivate()   -> _ZN6__asan12AsanActivateEv
 *   __asan::AsanDeactivate() -> _ZN6__asan14AsanDeactivateEv
 */
extern void _ZN6__asan12AsanActivateEv(void);
extern void _ZN6__asan14AsanDeactivateEv(void);
static inline void asan_activate(void)   { _ZN6__asan12AsanActivateEv(); }
static inline void asan_deactivate(void) { _ZN6__asan14AsanDeactivateEv(); }
#endif

#endif /* ASAN_ACTIVATION_H */
