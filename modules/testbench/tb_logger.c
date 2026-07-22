/*
 * Copyright (c) 2006-2021, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2026-07-15     12811       the first version
 */
#include <rtthread.h>
#include <testbench/tb.h>

#ifdef USE_PERF
#include <drivers/bsp_perf.h>
#endif

/* ---- 静态标志：表头是否已打印 ---- */
static uint8_t header_printed = 0;

/**
 * @brief  打印 CSV 表头（仅一次）
 */
static void print_csv_header(void)
{
    const TB_ExperimentInfo *info = tb_get_experiment_info();

    {
        int si = (int)info->fault_start_s;
        int sd = (int)((info->fault_start_s - si) * 10);
        if (sd < 0) sd = -sd;

        /* 元数据注释行 */
        rt_kprintf("# EXP=%d,SCN=%d,Fault=%s,Start=%d.%ds\r\n",
                   info->experiment_id,
                   info->scenario_id,
                   tb_fault_name[info->fault],
                   si, sd);
    }

    /* 表头行 */
    rt_kprintf("Time,Phase,Volt,Temp,DtTem,DvVol,DropR,TRisk,VRisk,ALM,HardFlt,SenFlt,FCause");
#ifdef USE_PERF
    rt_kprintf(",AcqUs,PredUs,SupUs,ActUs,EvtUs");
#endif
    rt_kprintf("\r\n");

    header_printed = 1;
}

/**
 * @brief  手动拆分浮点数为整数和小数部分
 * @param  val 浮点值
 * @param  int_part 输出：整数部分
 * @param  dec_part 输出：小数部分（2位）
 */
static void split_float(float val, int *int_part, int *dec_part)
{
    *int_part = (int)val;
    float frac = val - (float)(*int_part);
    *dec_part = (int)(frac * 100.0f);
    if (*dec_part < 0) *dec_part = -(*dec_part);
    if (*dec_part > 99) *dec_part = 99;
}

/**
 * @brief  输出一行 CSV 数据
 * @param  msg 监控消息快照
 */
void tb_log_csv(monitor_msg_t *msg)
{
    if (!header_printed)
    {
        print_csv_header();
    }

    /* 时间：3位小数（毫秒） */
    float time_s = tb_get_time();
    int time_ms = (int)(time_s * 1000.0f);
    int time_sec = (int)time_s;
    int time_frac = time_ms - time_sec * 1000;
    if (time_frac < 0) time_frac = -time_frac;

    /* Phase 从场景获取 */
    TB_PhysicsState scene;
    tb_scenario_get_state(time_s, &scene);
    int phase_val = (int)scene.phase;

    /* 电压：2位小数 */
    int volt_int, volt_dec;
    split_float(msg->voltage, &volt_int, &volt_dec);

    /* 温度：2位小数 */
    int temp_int, temp_dec;
    split_float(msg->temperature, &temp_int, &temp_dec);

    /* 温度微分：2位小数 */
    int dt_int, dt_dec;
    split_float(msg->dt_tem, &dt_int, &dt_dec);

    /* 电压微分：2位小数 */
    int dv_int, dv_dec;
    split_float(msg->dv_vol, &dv_int, &dv_dec);

    /* 压降比：3位小数 */
    int drop_int, drop_dec;
    float drop_1000 = msg->drop_ratio * 1000.0f;
    split_float(drop_1000, &drop_int, &drop_dec);

    /* 温度风险贡献：2位小数 */
    int trisk_int, trisk_dec;
    split_float(msg->temp_risk_contrib, &trisk_int, &trisk_dec);

    /* 电压风险贡献：2位小数 */
    int vrisk_int, vrisk_dec;
    split_float(msg->drop_risk_contrib, &vrisk_int, &vrisk_dec);

    /* 输出 CSV 行 */
    rt_kprintf("%d.%03d,%d,%d.%02d,%d.%02d,%d.%02d,%d.%02d,%d.%03d,%d.%02d,%d.%02d,%d,%d,%d,%d",
               time_sec, time_frac,
               phase_val,
               volt_int, volt_dec,
               temp_int, temp_dec,
               dt_int, dt_dec,
               dv_int, dv_dec,
               drop_int, drop_dec,
               trisk_int, trisk_dec,
               vrisk_int, vrisk_dec,
               msg->alarm_level,
               msg->hard_fault,
               msg->sensor_fault,
               msg->fault_cause);

#ifdef USE_PERF
    {
        /* CSV 输出各线程最近一次执行时间（last_us）+ Event 响应时间 */
        const perf_stat_t *acq  = perf_get_stat(PERF_ACQUIRE);
        const perf_stat_t *pred = perf_get_stat(PERF_PREDICT);
        const perf_stat_t *sup  = perf_get_stat(PERF_SUPERVISOR);
        const perf_stat_t *act  = perf_get_stat(PERF_ACTUATOR);
        uint32_t evt_us = perf_get_event_latency_us();
        rt_kprintf(",%u,%u,%u,%u,%u",
                   acq->last_us, pred->last_us, sup->last_us, act->last_us, evt_us);
    }
#endif

    rt_kprintf("\r\n");
}