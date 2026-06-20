#ifndef _BERTI_SIZE_H_
#define _BERTI_SIZE_H_

namespace berti_space
{
/*****************************************************************************
 *                              SIZES                                        *
 *****************************************************************************/

#define SIZE_1X

#if defined(SIZE_1X)
// MICRO Size
#define HISTORY_TABLE_SETS (8)
#define HISTORY_TABLE_WAYS (16)
#define TABLE_SET_MASK (0x7)

#define BERTI_TABLE_SIZE (16)
#define BERTI_TABLE_DELTA_SIZE (16)

#elif defined(SIZE_2X)
// 2xMICRO Size
#define HISTORY_TABLE_SETS (16)
#define HISTORY_TABLE_WAYS (32)
#define TABLE_SET_MASK (0xF)

#define BERTI_TABLE_SIZE (32)
#define BERTI_TABLE_DELTA_SIZE (32)

#elif defined(SIZE_4X)
// 4xMICRO Size
#define HISTORY_TABLE_SETS (32)
#define HISTORY_TABLE_WAYS (64)
#define TABLE_SET_MASK (0x1F)

#define BERTI_TABLE_SIZE (64)
#define BERTI_TABLE_DELTA_SIZE (64)

#elif defined(SIZE_050X)
// 0.5xMICRO Size
#define HISTORY_TABLE_SETS (4)
#define HISTORY_TABLE_WAYS (8)
#define TABLE_SET_MASK (0x3)

#define BERTI_TABLE_SIZE (8)
#define BERTI_TABLE_DELTA_SIZE (8)

#elif defined(SIZE_025X)
// 0.25xMICRO Size
#define HISTORY_TABLE_SETS (2)
#define HISTORY_TABLE_WAYS (4)
#define TABLE_SET_MASK (0x1)

#define BERTI_TABLE_SIZE (4)
#define BERTI_TABLE_DELTA_SIZE (4)

#endif

/*****************************************************************************
 *                              MASKS                                        *
 *****************************************************************************/
#define IP_MASK (0x3FF)
#define TIME_MASK (0xFFFF)
#define LAT_MASK (0xFFF)
#define ADDR_MASK (0xFFFFFF)
#define DELTA_MASK (12)

/*****************************************************************************
 *                      CONFIDENCE VALUES                                    *
 *****************************************************************************/
#define CONFIDENCE_MAX (16)
#define CONFIDENCE_INC (1)
#define CONFIDENCE_INIT (1)

#define CONFIDENCE_L1 (65)
#define CONFIDENCE_L2 (50)
#define CONFIDENCE_L2R (35)

/*****************************************************************************
 *                              LIMITS                                       *
 *****************************************************************************/
#define MAX_HISTORY_IP (8)
#define MAX_PF (16)
#define MAX_PF_LAUNCH (12)
#define MSHR_LIMIT (70)

/*****************************************************************************
 *                              CONSTANT PARAMETERS                          *
 *****************************************************************************/
#define BERTI_R (0x0)
#define BERTI_L1 (0x1)
#define BERTI_L2 (0x2)
#define BERTI_L2R (0x3)
}; // namespace berti_space
#endif
