/**
 * @file    diag_stage.h
 * @brief   启动阶段自诊断：跨复位记录并报告"应用卡在哪一步"。
 *
 * 解决的问题：
 *   现场没有 USB-TTL，模组的 DBG_UART 日志看不到；应用一旦在初始化阶段崩溃，
 *   表现就是"模组反复重启"，无法定位是哪一步。
 *
 * 做法：
 *   1) 每个初始化阶段执行前，把阶段号写入 Flash 文件 diagst.bin（"我正在做第 N 步"）；
 *   2) 阶段执行成功则记录 ok 位并清除"正在做"标记；
 *   3) 若某阶段把模组搞崩了，模组复位后重新启动，本模块发现"上次卡在第 N 步"，
 *      就把第 N 步记为 failed 并**跳过**它，继续后面的阶段；
 *   4) 于是崩溃循环会自行收敛：跳过所有崩溃阶段后应用最终稳定运行，
 *      而失败阶段列表可以通过 boot_probe 的 8081 页面读出来。
 *
 * 这个模块只在"诊断固件"里使用；正常固件不需要它（但保留无害）。
 */
#ifndef DIAG_STAGE_H
#define DIAG_STAGE_H

#include <stdint.h>

#define DIAG_STAGE_MAX   16
#define DIAG_STAGE_NONE  0xFF

/** 从 Flash 读出诊断状态到内存（首次调用会自动处理"上次崩溃"）。 */
void diag_load(void);

/** 处理上一次启动的崩溃：把 attempted 阶段记入 failed 并落盘。 */
void diag_note_boot(void);

/** 该阶段是否还没被标记为崩溃（1=需要执行）。 */
int  diag_should_run(int stage);

/** 标记"正在执行第 stage 步"并落盘（必须在执行前调用）。 */
void diag_enter(int stage);

/** 标记第 stage 步执行成功并落盘。 */
void diag_ok(int stage);

/** 全部阶段结束：清除"正在执行"标记（避免下次启动误判崩溃）。 */
void diag_finish(void);

/** 清空全部诊断记录（人工复位用）。 */
void diag_clear(void);

/** 上次卡住的阶段号（DIAG_STAGE_NONE = 无）。 */
int      diag_attempted(void);
/** 崩溃过的阶段位图。 */
uint32_t diag_failed_mask(void);
/** 成功过的阶段位图。 */
uint32_t diag_ok_mask(void);
/** 崩溃次数。 */
int      diag_crashes(void);
/** 阶段名（用于报告）。 */
const char *diag_stage_name(int stage);
/** 生成可读报告文本，返回写入长度。 */
int      diag_report(char *buf, uint32_t buf_size);

#endif /* DIAG_STAGE_H */
