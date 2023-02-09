DEVICE=attiny85
CLOCK=1000000
PROGRAMMER=-c usbasp

# for ATTiny85
# see http://www.engbedded.com/fusecalc/
FUSES=-U lfuse:w:0x62:m -U hfuse:w:0xdf:m -U efuse:w:0xff:m

AVRDUDE = avrdude $(PROGRAMMER) -p $(DEVICE)
COMPILE = avr-gcc -Wall -Os -DF_CPU=$(CLOCK) -mmcu=$(DEVICE)

default:
	# compile for attiny86 with warnings, optimizations, and 1 MHz clock frequency
	$(COMPILE) -o leah_sign.o leah_sign.c
	avr-size --format=avr --mcu=$(DEVICE) leah_sign.o
	avr-objcopy -j .text -j .data -O ihex leah_sign.o leah_sign.hex
	avr-objcopy -j .eeprom --change-section-lma .eeprom=0 -O ihex leah_sign.o leah_sign.eep

fuse:
	$(AVRDUDE) $(FUSES)

flash:
	$(AVRDUDE) -U flash:w:leah_sign.hex:i
	$(AVRDUDE) -U eeprom:w:leah_sign.eep:i


clean: /dev/null
	- rm leah_sign.o leah_sign.hex leah_sign.eep
