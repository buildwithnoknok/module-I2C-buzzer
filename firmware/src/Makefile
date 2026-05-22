PROJECT = buzzer_firmware
SOURCES = buzzer_firmware.c
TARGET:=buzzer_firmware

TARGET_MCU?=CH32V003

include ../ch32fun/ch32fun.mk

all : flash
flash : cv_flash
clean : cv_clean
