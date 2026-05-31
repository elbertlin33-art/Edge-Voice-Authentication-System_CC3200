#include "pir_sensor.h"
#include "hw_types.h"
#include "hw_memmap.h"
#include "hw_gpio.h"
#include "gpio.h"

#define PIR_GPIO_BASE GPIOA3_BASE
#define PIR_GPIO_PIN  0x80

void PIR_Init(void)
{
    GPIODirModeSet(PIR_GPIO_BASE, PIR_GPIO_PIN, GPIO_DIR_MODE_IN);
}

int PIR_MotionDetected(void)
{
    return (GPIOPinRead(PIR_GPIO_BASE, PIR_GPIO_PIN) != 0) ? 1 : 0;
}
