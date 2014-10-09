/*
	Copyright (c) 2014 CurlyMo <curlymoo1@gmail.com>

	This program is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <ctype.h>
#include <poll.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/ioctl.h>

#include "wiringX.h"
#include "i2c-dev.h"
#include "bananapi.h"

#define	WPI_MODE_PINS		 0
#define	WPI_MODE_GPIO		 1
#define	WPI_MODE_GPIO_SYS	 2
#define	WPI_MODE_PHYS		 3
#define	WPI_MODE_PIFACE		 4
#define	WPI_MODE_UNINITIALISED	-1

#define	PAGE_SIZE		(4*1024)
#define	BLOCK_SIZE		(4*1024)

#define MAP_SIZE		(4096*2)
#define MAP_MASK		(MAP_SIZE - 1)

#define	PI_GPIO_MASK	(0xFFFFFFC0)

#define SUNXI_GPIO_BASE		(0x01C20800)
#define GPIO_BASE_BP		(0x01C20000)

#define	NUM_PINS		0x40

static int wiringPiMode = WPI_MODE_UNINITIALISED;

static volatile uint32_t *gpio;

static int pinModes[NUM_PINS];

static int BP_PIN_MASK[9][32] = {
	{-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, }, //PA
	{-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 20, 21, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, }, //PB
	{-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, }, //PC
	{-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, }, //PD
	{-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, }, //PE
	{-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, }, //PF
	{-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, }, //PG
	{0, 1, 2, 3, -1, 5, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 20, 21, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, }, //PH
	{-1, -1, -1, 3, -1, -1, -1, -1, -1, -1, 10, 11, 12, 13, 14, -1, 16, 17, 18, 19, 20, 21, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, }, //PI
};

static int edge[64] = {
	-1, -1, -1, -1, -1, -1, -1, 7,
	8, 9, 10, 11, -1,-1, 14, 15,
	-1, 17, 18, -1, -1, 21, 22, 23,
	24, 25, -1, 27, 28, -1, 30, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
};

static int sysFds[64] = {
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
};

static int *pinToGpio;

static int pinToGpioR2[64] = {
	17, 18, 27, 22, 23, 24, 25, 4,	// From the Original Wiki - GPIO 0 through 7:	wpi  0 -  7
	2, 3,							// I2C  - SDA0, SCL0							wpi  8 -  9
	8, 7,							// SPI  - CE1, CE0								wpi	10 - 11
	10, 9, 11, 						// SPI  - MOSI, MISO, SCLK						wpi 12 - 14
	14, 15,							// UART - Tx, Rx								wpi 15 - 16
	28, 29, 30, 31,					// New GPIOs 8 though 11						wpi 17 - 20
	// Padding:
						-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,	// ... 31
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,	// ... 47
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,	// ... 63
};

static int pinToGpioR3[64] = {
	275, 226,
	274, 273,
	244, 245,
	272, 259,
	53, 52,
	266, 270,
	268, 269,
	267, 224,
	225, 229,
	277, 227,
	276,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, // ... 31
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, // ... 47
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, // ... 63
};

static int pinToBCMR3[64] = {
	53, 52,
	53, 52,
	259, -1,
	-1, 270,
	266, 269,//9
	268, 267,
	-1, -1,
	224, 225,
	-1, 275,
	226, -1,//19
	-1,
	274, 273, 244, 245, 272, -1, 274, 229, 277, 227, // ... 31
	276, -1,-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, // ... 47
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, // ... 63
};

static int *physToGpio;

static int physToGpioR3[64] = {
	-1,  // 0
	-1, -1, // 1, 2
	53, -1,
	52, -1,
	259, 224,
	-1, 225,
	275, 226,
	274, -1,
	273, 244,
	-1, 245,
	268, -1,
	269, 272,
	267, 266,
	-1, 270, // 25, 26
	-1, -1,
	229, 277,
	227, 276,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, // ... 48
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, // ... 63
};

uint32_t readl(uint32_t addr) {
	uint32_t val = 0;
	uint32_t mmap_base = (addr & ~MAP_MASK);
	uint32_t mmap_seek = ((addr - mmap_base) >> 2);
	val = *(gpio + mmap_seek);
	return val;
}

void writel(uint32_t val, uint32_t addr) {
	uint32_t mmap_base = (addr & ~MAP_MASK);
	uint32_t mmap_seek = ((addr - mmap_base) >> 2);
	*(gpio + mmap_seek) = val;
}

static int changeOwner(char *file) {
	uid_t uid = getuid();
	uid_t gid = getgid();

	if(chown(file, uid, gid) != 0) {
		if(errno == ENOENT)	{
			fprintf(stderr, "bananapi->changeOwner: File not present: %s\n", file);
			return -1;
		} else {
			fprintf(stderr, "bananapi->changeOwner: Unable to change ownership of %s: %s\n", file, strerror (errno));
			return -1;
		}
	}

	return 0;
}

static int bananapiISR(int pin, int mode) {
	int i = 0, fd = 0, match = 0, count = 0;
	const char *sMode = NULL;
	char path[30], c;

	pinModes[pin] = SYS;

	if(mode == INT_EDGE_FALLING) {
		sMode = "falling" ;
	} else if(mode == INT_EDGE_RISING) {
		sMode = "rising" ;
	} else if(mode == INT_EDGE_BOTH) {
		sMode = "both";
	} else {
		fprintf(stderr, "bananapi->isr: Invalid mode. Should be INT_EDGE_BOTH, INT_EDGE_RISING, or INT_EDGE_FALLING\n");
		return -1;
	}


	FILE *f = NULL;
	for(i=0;i<NUM_PINS;i++) {
		if(pin == i) {
			sprintf(path, "/sys/class/gpio/gpio%d/value", pinToGpio[i]);
			fd = open(path, O_RDWR);
			match = 1;
		}
	}

	if(edge[pinToGpio[pin]] == -1) {
		// Not supported
		return -1;
	}

	if(!match) {
		fprintf(stderr, "bananapi->isr: Invalid GPIO: %d\n", pin);
		exit(0);
	}

	if(fd < 0) {
		if((f = fopen("/sys/class/gpio/export", "w")) == NULL) {
			fprintf(stderr, "bananapi->isr: Unable to open GPIO export interface\n");
			exit(0);
		}

		fprintf(f, "%d\n", pinToGpio[pin]);
		fclose(f);
	}

	sprintf(path, "/sys/class/gpio/gpio%d/direction", pinToGpio[pin]);
	if((f = fopen(path, "w")) == NULL) {
		fprintf(stderr, "bananapi->isr: Unable to open GPIO direction interface for pin %d: %s\n", pin, strerror(errno));
		return -1;
	}

	fprintf(f, "in\n");
	fclose(f);

	sprintf(path, "/sys/class/gpio/gpio%d/edge", pinToGpio[pin]);
	if((f = fopen(path, "w")) == NULL) {
		fprintf(stderr, "bananapi->isr: Unable to open GPIO edge interface for pin %d: %s\n", pin, strerror(errno));
		return -1;
	}

	if(strcasecmp(sMode, "none") == 0) {
		fprintf(f, "none\n");
	} else if(strcasecmp(sMode, "rising") == 0) {
		fprintf(f, "rising\n");
	} else if(strcasecmp(sMode, "falling") == 0) {
		fprintf(f, "falling\n");
	} else if(strcasecmp (sMode, "both") == 0) {
		fprintf(f, "both\n");
	} else {
		fprintf(stderr, "bananapi->isr: Invalid mode: %s. Should be rising, falling or both\n", sMode);
		return -1;
	}

	sprintf(path, "/sys/class/gpio/gpio%d/value", pinToGpio[pin]);
	if((sysFds[pin] = open(path, O_RDONLY)) < 0) {
		fprintf(stderr, "bananapi->isr: Unable to open GPIO value interface: %s\n", strerror(errno));
		return -1;
	}
	changeOwner(path);

	sprintf(path, "/sys/class/gpio/gpio%d/edge", pinToGpio[pin]);
	changeOwner(path);

	fclose(f);

	ioctl(fd, FIONREAD, &count);
	for(i=0; i<count; ++i) {
		read(fd, &c, 1);
	}
	close(fd);

	return 0;
}

static int bananapiWaitForInterrupt(int pin, int ms) {
	int x = 0;
	uint8_t c = 0;
	struct pollfd polls;

	if(pinModes[pin] != SYS) {
		fprintf(stderr, "bananapi->waitForInterrupt: Trying to read from pin %d, but it's not configured as interrupt\n", pin);
		return -1;
	}

	if(sysFds[pin] == -1) {
		fprintf(stderr, "bananapi->waitForInterrupt: GPIO %d not set as interrupt\n", pin);
		return -1;
	}

	polls.fd = sysFds[pin];
	polls.events = POLLPRI;

	x = poll(&polls, 1, ms);

	(void)read(sysFds[pin], &c, 1);
	lseek(sysFds[pin], 0, SEEK_SET);

	return x;
}

static int piBoardRev(void) {
	FILE *cpuFd;
	char line[120];
	char *d;

	if((cpuFd = fopen("/proc/cpuinfo", "r")) == NULL) {
		fprintf(stderr, "bananapi->identify: Unable open /proc/cpuinfo\n");
		return -1;
	}

	while(fgets(line, 120, cpuFd) != NULL) {
		if(strncmp(line, "Hardware", 8) == 0) {
			break;
		}
	}

	fclose(cpuFd);

	if(strncmp(line, "Hardware", 8) != 0) {
		fprintf(stderr, "bananapi->identify: /proc/cpuinfo has no hardware line\n");
		return -1;
	}

	for(d = &line[strlen(line) - 1]; (*d == '\n') || (*d == '\r') ; --d)
		*d = 0 ;

	if(strstr(line, "sun7i") != NULL) {
		return 0;
	} else {
		return -1;
	}
}

static int setup(void)	{
	int fd;
	int boardRev;

	boardRev = piBoardRev();
	if(boardRev == 0) {
		pinToGpio = pinToGpioR2;
		physToGpio = physToGpioR3;
	}

	if((fd = open ("/dev/mem", O_RDWR | O_SYNC | O_CLOEXEC) ) < 0) {
		fprintf(stderr, "bananapi->setup: Unable to open /dev/mem\n");
		return -1;
	}

	if(boardRev == 0) {
		gpio = (uint32_t *)mmap(0, BLOCK_SIZE, PROT_READ|PROT_WRITE, MAP_SHARED, fd, GPIO_BASE_BP);

		if((int32_t)gpio == -1) {
			fprintf(stderr, "bananapi->setup: mmap (GPIO) failed\n");
			return -1;
		}
	}

	wiringPiMode = WPI_MODE_PINS;

	return 0;
}

static int bananapiDigitalRead(int pin) {
	uint32_t regval = 0, phyaddr = 0;
	int bank = 0, i = 0, offset = 0;

	if(pinModes[pin] != INPUT) {
		fprintf(stderr, "bananapi->digitalRead: Trying to write to pin %d, but it's not configured as input\n", pin);
		return -1;
	}

	if((pin & PI_GPIO_MASK) == 0) {
		if(wiringPiMode == WPI_MODE_PINS)
			pin = pinToGpioR3[pin];
		else if (wiringPiMode == WPI_MODE_PHYS)
			pin = physToGpioR3[pin];
		else if(wiringPiMode == WPI_MODE_GPIO)
			pin = pinToBCMR3[pin]; //need map A20 to bcm
		else
			return -1;

		regval = 0;
		bank = pin >> 5;
		i = pin - (bank << 5);
		phyaddr = SUNXI_GPIO_BASE + (bank * 36) + 0x10; // +0x10 -> data reg

		if(BP_PIN_MASK[bank][i] != -1) {
			regval = readl(phyaddr);
			regval = regval >> i;
			regval &= 1;
			return regval;
		}
	}
	return 0;
}

static int bananapiDigitalWrite(int pin, int value) {
	uint32_t regval = 0, phyaddr = 0;
	int bank = 0, i = 0, offset = 0;

	if(pinModes[pin] != OUTPUT) {
		fprintf(stderr, "bananapi->digitalWrite: Trying to write to pin %d, but it's not configured as output\n", pin);
		return -1;
	}

	if((pin & PI_GPIO_MASK) == 0) {
		if(wiringPiMode == WPI_MODE_PINS)
			pin = pinToGpioR3[pin];
		else if (wiringPiMode == WPI_MODE_PHYS)
			pin = physToGpioR3[pin];
		else if(wiringPiMode == WPI_MODE_GPIO)
			pin = pinToBCMR3[pin]; //need map A20 to bcm
		else
			return -1;

		regval = 0;
		bank = pin >> 5;
		i = pin - (bank << 5);
		phyaddr = SUNXI_GPIO_BASE + (bank * 36) + 0x10; // +0x10 -> data reg

		if(BP_PIN_MASK[bank][i] != -1) {
			regval = readl(phyaddr);

			if(value == LOW) {
				regval &= ~(1 << i);
				writel(regval, phyaddr);
				regval = readl(phyaddr);
			} else {
				regval |= (1 << i);
				writel(regval, phyaddr);
				regval = readl(phyaddr);
			}
		}
	}
	return 0;
}

static int bananapiPinMode(int pin, int mode) {
	uint32_t regval = 0, phyaddr = 0;
	int bank = 0, i = 0, offset = 0;

	if((pin & PI_GPIO_MASK) == 0) {
		if(wiringPiMode == WPI_MODE_PINS)
			pin = pinToGpioR3[pin] ;
		else if(wiringPiMode == WPI_MODE_PHYS)
			pin = physToGpioR3[pin] ;
		else if(wiringPiMode == WPI_MODE_GPIO)
			pin=pinToBCMR3[pin]; //need map A20 to bcm
		else
			return -1;

		regval = 0;
		bank = pin >> 5;
		i = pin - (bank << 5);
		offset = ((i - ((i >> 3) << 3)) << 2);
		phyaddr = SUNXI_GPIO_BASE + (bank * 36) + ((i >> 3) << 2);

		if(BP_PIN_MASK[bank][i] != -1) {
			pinModes[pin] = mode;
			regval = readl(phyaddr);

			if(mode == INPUT) {
				regval &= ~(7 << offset);
				writel(regval, phyaddr);
				regval = readl(phyaddr);
			} else if(mode == OUTPUT) {
			   regval &= ~(7 << offset);
			   regval |= (1 << offset);
			   writel(regval, phyaddr);
			   regval = readl(phyaddr);
			} else {
				return -1;
			}
		}
	}
	return 0;
}

static int bananapiGC(void) {
	int i = 0, fd = 0;
	char path[30];
	FILE *f = NULL;

	for(i=0;i<NUM_PINS;i++) {
		if(wiringPiMode == WPI_MODE_PINS || wiringPiMode == WPI_MODE_PHYS || wiringPiMode != WPI_MODE_GPIO) {
			pinMode(i, INPUT);
		}
		sprintf(path, "/sys/class/gpio/gpio%d/value", pinToGpio[i]);
		if((fd = open(path, O_RDWR)) > 0) {
			if((f = fopen("/sys/class/gpio/unexport", "w")) == NULL) {
				fprintf(stderr, "bananapi->gc: Unable to open GPIO unexport interface: %s\n", strerror(errno));
			}

			fprintf(f, "%d\n", pinToGpio[i]);
			fclose(f);
			close(fd);
		}
		if(sysFds[i] > 0) {
			close(sysFds[i]);
		}
	}

	if(gpio) {
		munmap((void *)gpio, BLOCK_SIZE);
	}
	return 0;
}


int bananapiI2CRead(int fd) {
	return i2c_smbus_read_byte(fd);
}

int bananapiI2CReadReg8(int fd, int reg) {
	return i2c_smbus_read_byte_data(fd, reg);
}

int bananapiI2CReadReg16(int fd, int reg) {
	return i2c_smbus_read_word_data(fd, reg);
}

int bananapiI2CWrite(int fd, int data) {
	return i2c_smbus_write_byte(fd, data);
}

int bananapiI2CWriteReg8(int fd, int reg, int data) {
	return i2c_smbus_write_byte_data(fd, reg, data);
}

int bananapiI2CWriteReg16(int fd, int reg, int data) {
	return i2c_smbus_write_word_data(fd, reg, data);
}

int bananapiI2CSetup(int devId) {
	int rev = 0, fd = 0;
	const char *device = NULL;

	if((rev = piBoardRev()) < 0) {
		fprintf(stderr, "bananapi->I2CSetup: Unable to determine Pi board revision\n");
		return -1;
	}

	if(rev == 0)
		device = "/dev/i2c-2";
	else
		device = "/dev/i2c-3";

	if((fd = open(device, O_RDWR)) < 0) {
		fprintf(stderr, "bananapi->I2CSetup: Unable to open %s\n", device);
		return -1;
	}

	if(ioctl(fd, I2C_SLAVE, devId) < 0) {
		fprintf(stderr, "bananapi->I2CSetup: Unable to set %s to slave mode\n", device);
		return -1;
	}

	return fd;
}

void bananapiInit(void) {

	memset(pinModes, -1, NUM_PINS);

	device_register(&bananapi, "bananapi");
	bananapi->setup=&setup;
	bananapi->pinMode=&bananapiPinMode;
	bananapi->digitalWrite=&bananapiDigitalWrite;
	bananapi->digitalRead=&bananapiDigitalRead;
	bananapi->identify=&piBoardRev;
	bananapi->isr=&bananapiISR;
	bananapi->waitForInterrupt=&bananapiWaitForInterrupt;
	bananapi->I2CRead=&bananapiI2CRead;
	bananapi->I2CReadReg8=&bananapiI2CReadReg8;
	bananapi->I2CReadReg16=&bananapiI2CReadReg16;
	bananapi->I2CWrite=&bananapiI2CWrite;
	bananapi->I2CWriteReg8=&bananapiI2CWriteReg8;
	bananapi->I2CWriteReg16=&bananapiI2CWriteReg16;
	bananapi->I2CSetup=&bananapiI2CSetup;
	bananapi->gc=&bananapiGC;
}