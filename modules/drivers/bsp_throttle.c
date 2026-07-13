/*
 * Copyright (c) 2006-2021, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2026-05-03     12811       the first version
 */
#include "bsp_throttle.h"


//返回值 1000 代表"空闲"状态（油门通道中立值通常是 1000~2000，1000 为最低）
uint16_t throttle_read(void)
{
    return 1000;
}