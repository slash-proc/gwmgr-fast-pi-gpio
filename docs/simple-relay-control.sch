EESchema Schematic File Version 4
LIBS:simple-relay-control-cache
EELAYER 29 0
EELAYER END
$Descr A4 11693 8268
Sheet 1 1
Title "Simple USB / Battery Relay and NRST Control"
Date "2026-10-14"
Rev "1"
Comp "Reference design"
Comment1 "K1 USB power, K2 battery power, Q3 open-drain NRST"
Comment2 "Keep all grounds common for SWD"
Comment3 "Import this legacy schematic in KiCad and save as .kicad_sch"
Comment4 "Do not close K1 and K2 together on one target rail"
$EndDescr
$Comp
L simple-relay-control-cache:CONN_2 J1
U 1 1 1
P 1000 1200
F 0 "J1" H 918 1517 50  0000 C CNN
F 1 "USB_INPUT" H 918 1426 50 0000 C CNN
 1    1000 1200
 1    0    0    -1
$EndComp
$Comp
L simple-relay-control-cache:RELAY_SPDT K1
U 1 1 2
P 2500 1200
F 0 "K1" H 2500 1630 50 0000 C CNN
F 1 "USB_POWER_RELAY" H 2500 1540 50 0000 C CNN
 1    2500 1200
 1    0    0    -1
$EndComp
$Comp
L simple-relay-control-cache:CONN_2 J2
U 1 1 3
P 1000 2900
F 0 "J2" H 918 3217 50 0000 C CNN
F 1 "BATTERY" H 918 3126 50 0000 C CNN
 1    1000 2900
 1    0    0    -1
$EndComp
$Comp
L simple-relay-control-cache:RELAY_SPDT K2
U 1 1 4
P 2500 2900
F 0 "K2" H 2500 3330 50 0000 C CNN
F 1 "BATTERY_POWER_RELAY" H 2500 3240 50 0000 C CNN
 1    2500 2900
 1    0    0    -1
$EndComp
$Comp
L simple-relay-control-cache:TARGET_7 J3
U 1 1 5
P 7600 1900
F 0 "J3" H 7600 2417 50 0000 C CNN
F 1 "TARGET_SWD" H 7600 2326 50 0000 C CNN
 1    7600 1900
 1    0    0    -1
$EndComp
$Comp
L simple-relay-control-cache:CONN_5 J4
U 1 1 6
P 1500 5000
F 0 "J4" H 1418 5417 50 0000 C CNN
F 1 "PI_CONTROL" H 1418 5326 50 0000 C CNN
 1    1500 5000
 1    0    0    -1
$EndComp
$Comp
L simple-relay-control-cache:RES_H R1
U 1 1 7
P 3200 4900
F 0 "R1" V 2993 4900 50 0000 C CNN
F 1 "1k" V 3084 4900 50 0000 C CNN
 1    3200 4900
 1    0    0    -1
$EndComp
$Comp
L simple-relay-control-cache:RES_H R2
U 1 1 8
P 3200 5200
F 0 "R2" V 2993 5000 50 0000 C CNN
F 1 "1k" V 3084 5000 50 0000 C CNN
 1    3200 5000
 1    0    0    -1
$EndComp
$Comp
L simple-relay-control-cache:RES_H R3
U 1 1 9
P 3200 5100
F 0 "R3" V 2993 5100 50 0000 C CNN
F 1 "1k" V 3084 5100 50 0000 C CNN
 1    3200 5100
 1    0    0    -1
$EndComp
$Comp
L simple-relay-control-cache:NMOS Q1
U 1 1 10
P 4100 4900
F 0 "Q1" H 4220 5080 50 0000 L CNN
F 1 "2N7000" H 4220 4990 50 0000 L CNN
 1    4100 4900
 1    0    0    -1
$EndComp
$Comp
L simple-relay-control-cache:NMOS Q2
U 1 1 11
P 5600 5200
F 0 "Q2" H 5720 5380 50 0000 L CNN
F 1 "2N7000" H 5720 5290 50 0000 L CNN
 1    5600 5200
 1    0    0    -1
$EndComp
$Comp
L simple-relay-control-cache:NMOS Q3
U 1 1 12
P 7100 5500
F 0 "Q3" H 7220 5680 50 0000 L CNN
F 1 "BSS138" H 7220 5590 50 0000 L CNN
 1    7100 5500
 1    0    0    -1
$EndComp
$Comp
L simple-relay-control-cache:RES_V R4
U 1 1 13
P 4100 5900
F 0 "R4" H 4170 5946 50 0000 L CNN
F 1 "100k" H 4170 5855 50 0000 L CNN
 1    4100 5900
 1    0    0    -1
$EndComp
$Comp
L simple-relay-control-cache:RES_V R5
U 1 1 14
P 5600 6200
F 0 "R5" H 5670 6246 50 0000 L CNN
F 1 "100k" H 5670 6155 50 0000 L CNN
 1    5600 6200
 1    0    0    -1
$EndComp
$Comp
L simple-relay-control-cache:RES_V R6
U 1 1 15
P 7800 5000
F 0 "R6" H 7870 5046 50 0000 L CNN
F 1 "10k" H 7870 4955 50 0000 L CNN
 1    7800 5000
 1    0    0    -1
$EndComp
$Comp
L simple-relay-control-cache:DIODE_V D1
U 1 1 16
P 2950 1200
F 0 "D1" H 3050 1246 50 0000 L CNN
F 1 "1N4007" H 3050 1155 50 0000 L CNN
 1    2950 1200
 1    0    0    -1
$EndComp
$Comp
L simple-relay-control-cache:DIODE_V D2
U 1 1 17
P 2950 2900
F 0 "D2" H 3050 2946 50 0000 L CNN
F 1 "1N4007" H 3050 2855 50 0000 L CNN
 1    2950 2900
 1    0    0    -1
$EndComp
Wire Wire Line
  1400 1120 1800 1120
Wire Wire Line
  1800 1120 1800 1100
Wire Wire Line
  1800 1100 2000 1100
Wire Wire Line
  3000 1100 5000 1100
Wire Wire Line
  1400 1280 1400 6800
Wire Wire Line
  1400 2820 1800 2820
Wire Wire Line
  1800 2820 1800 2800
Wire Wire Line
  1800 2800 2000 2800
Wire Wire Line
  3000 2800 5000 2800
Wire Wire Line
  1400 2980 1400 6800
Wire Wire Line
  2950 950 2950 1000
Wire Wire Line
  2950 2650 2950 2700
Wire Wire Line
  7100 1100 7100 1600
Wire Wire Line
  7100 2800 7100 1700
Text Label 1500 1120 0    50   ~ 0
USB_5V
Text Label 1500 1280 0    50   ~ 0
COMMON_GND
Text Label 1500 2820 0    50   ~ 0
BAT+
Text Label 1500 2980 0    50   ~ 0
COMMON_GND
Text Label 3500 1100 0    50   ~ 0
TARGET_USB_5V
Text Label 3500 2800 0    50   ~ 0
TARGET_BAT+
Text Label 2950 950 0    50   ~ 0
PI_5V
Text Label 2000 1450 0    50   ~ 0
PI_5V
Text Label 3000 1450 0    50   ~ 0
K1_COIL_LOW
Text Label 2950 2650 0    50   ~ 0
PI_5V
Text Label 2000 3150 0    50   ~ 0
PI_5V
Text Label 3000 3150 0    50   ~ 0
K2_COIL_LOW
Text Label 7100 1600 0    50   ~ 0
TARGET_USB_5V
Text Label 7100 1700 0    50   ~ 0
TARGET_BAT+
Text Label 7100 1800 0    50   ~ 0
TARGET_IOVDD
Text Label 7100 1900 0    50   ~ 0
TARGET_NRST
Text Label 1900 4800 0    50   ~ 0
PI_5V
Wire Wire Line
  1900 4900 3050 4900
Wire Wire Line
  1900 5000 3050 5000
Wire Wire Line
  1900 5100 3050 5100
Wire Wire Line
  1900 5200 1900 6800
Wire Wire Line
  3500 4900 3750 4900
Wire Wire Line
  3500 5000 4800 5000
Wire Wire Line
  4800 5000 4800 5200
Wire Wire Line
  4800 5200 5250 5200
Wire Wire Line
  3500 5100 6400 5100
Wire Wire Line
  6400 5100 6400 5500
Wire Wire Line
  6400 5500 6750 5500
Wire Wire Line
  4100 4550 4100 4700
Wire Wire Line
  5600 4850 5600 5000
Wire Wire Line
  7100 5150 7100 5300
Wire Wire Line
  4100 5250 4100 5600
Wire Wire Line
  4100 5600 4100 5750
Wire Wire Line
  4100 6200 4100 6800
Wire Wire Line
  5600 5550 5600 5900
Wire Wire Line
  5600 5900 5600 6050
Wire Wire Line
  5600 6500 5600 6800
Wire Wire Line
  7100 5850 7100 6800
Wire Wire Line
  7100 5150 7800 5150
Wire Wire Line
  7800 5300 7800 5150
Text Label 3500 4900 0    50   ~ 0
GPIO5
Text Label 3500 5000 0    50   ~ 0
GPIO6
Text Label 3500 5100 0    50   ~ 0
GPIO26
Text Label 4100 4550 0    50   ~ 0
K1_COIL_LOW
Text Label 5600 4850 0    50   ~ 0
K2_COIL_LOW
Text Label 7800 4700 0    50   ~ 0
TARGET_IOVDD
Text Label 7100 5150 0    50   ~ 0
TARGET_NRST
Text Label 1400 6800 0    50   ~ 0
COMMON_GND
Text Notes 900 850 0    80   ~ 16
USB +5 V and battery positive are independently switched by K1 and K2.
Text Notes 900 3700 0    60   ~ 12
Keep the grounds common. Do not close K1 and K2 together if their outputs feed one rail.
Text Notes 900 4000 0    60   ~ 12
Q1/Q2 sink the relay coils; D1/D2 are flyback diodes with cathodes toward PI_5V.
Text Notes 900 4300 0    60   ~ 12
Q3 pulls NRST low. R6 is the target NRST pull-up to TARGET_IOVDD.
$EndSCHEMATC
