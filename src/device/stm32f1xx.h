/**
  ******************************************************************************
  * @file    stm32f1xx.h
  * @brief   CMSIS STM32F1xx Device Peripheral Access Layer Header File.
  *
  *          Minimal header for jOS RTOS port — dispatches to the F103 variant
  *          header based on the STM32F103xx preprocessor define.
  ******************************************************************************
  */
#ifndef __STM32F1xx_H
#define __STM32F1xx_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/** @addtogroup Library_configuration_section
  * @{
  */

#if !defined (STM32F1)
#define STM32F1
#endif /* STM32F1 */

#if !defined (STM32F103xx)
#error "Please select the target STM32F1xx device: define STM32F103xx in compiler preprocessor"
#endif

/* Include the variant-specific header */
#include "stm32f103xx.h"

/**
  * @}
  */

/** @addtogroup Exported_types
  * @{
  */
typedef enum { RESET = 0U, SET = !RESET } FlagStatus, ITStatus;
typedef enum { DISABLE = 0U, ENABLE = !DISABLE } FunctionalState;
typedef enum { SUCCESS = 0U, ERROR = !SUCCESS } ErrorStatus;
/**
  * @}
  */

/** @addtogroup Exported_macro
  * @{
  */
#define SET_BIT(REG, BIT)     ((REG) |= (BIT))
#define CLEAR_BIT(REG, BIT)   ((REG) &= ~(BIT))
#define READ_BIT(REG, BIT)    ((REG) & (BIT))
#define CLEAR_REG(REG)        ((REG) = (0x0))
#define WRITE_REG(REG, VAL)   ((REG) = (VAL))
#define READ_REG(REG)         ((REG))
#define MODIFY_REG(REG, CLEARMASK, SETMASK)  WRITE_REG((REG), (((READ_REG(REG)) & (~(CLEARMASK))) | (SETMASK)))
#define POSITION_VAL(VAL)     (__CLZ(__RBIT(VAL)))
/**
  * @}
  */

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* __STM32F1xx_H */