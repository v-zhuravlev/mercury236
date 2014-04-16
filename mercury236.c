/*
 *	Mercury 236 power meter communication utility.
 *
 *	RS485 USB dongle is used to connect to the power meter and to collect grid power measures
 *	including voltage, current, consumption power, counters, cos(f) etc.
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <fcntl.h>
#include <termios.h>
#include <stdio.h>
#include <strings.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>

#pragma pack(1)
#define BAUDRATE 	B9600		// 9600 baud
#define MODEMDEVICE 	"/dev/ttyUSB0"	// Dongle device
#define _POSIX_SOURCE 	1		// POSIX compliant source
#define UInt16		uint16_t
#define byte		unsigned char
#define TIME_OUT	50		// Mercury inter-command delay (ms)
#define CH_TIME_OUT	2		// Channel timeout (sec)
#define BSZ		255
#define PM_ADDRESS	0		// RS485 addess of the power meter
#define OPT_DEBUG	"--debug"
#define OPT_HELP	"--help"

int debugPrint = 0;

// ***** Commands
// Test connection
typedef struct
{
	byte	address;
	byte	command;
	UInt16	CRC;
} TestCmd;

// Connection initialisaton command
typedef struct
{
	byte	address;
	byte	command;
	byte 	accessLevel;
	byte	password[6];
	UInt16	CRC;
} InitCmd;

// Connection terminaion command
typedef struct
{
	byte	address;
	byte	command;
	UInt16	CRC;
} ByeCmd;

// Power meter parameters read command
typedef struct
{
	byte	address;
	byte	command;	// 8h
	byte	paramId;	// No of parameter to read
	byte	BWRI;
	UInt16 	CRC;
} ReadParamCmd;

// ***** Results
// 1-byte responce (usually with status code)
typedef struct
{
	byte	address;
	byte	result;
	UInt16	CRC;
} Result_1b;

// 3-byte responce
typedef struct
{
	byte	address;
	byte	res[3];
	UInt16	CRC;
} Result_3b;

// Result with 3 bytes per phase
typedef struct
{
	byte	address;
	byte	p1[3];
	byte	p2[3];
	byte	p3[3];
	UInt16	CRC;
} Result_3x3b;

// Result with 3 bytes per phase plus 3 bytes for phases sum
typedef struct
{
	byte	address;
	byte	sum[3];
	byte	p1[3];
	byte	p2[3];
	byte	p3[3];
	UInt16	CRC;
} Result_4x3b;

// Result with 4 bytes per phase plus 4 bytes for sum
typedef struct
{
	byte	address;
	byte	sum[4];
	byte	p1[4];
	byte	p2[4];
	byte	p3[4];
	UInt16	CRC;
} Result_4x4b;

// 3-phase vector (for voltage, frequency, power by phases)
typedef struct
{
	float	p1;
	float	p2;
	float	p3;
} P3V;

// 3-phase vector (for voltage, frequency, power by phases) with sum by all phases
typedef struct
{
	float	sum;
	float	p1;
	float	p2;
	float	p3;
} P3VS;

// **** Enums
typedef enum
{
	OUT = 0,
	IN = 1
} Direction;

typedef enum
{
	OK = 0,
	ILLEGAL_CMD = 1,
	INTERNAL_COUNTER_ERR = 2,
	PERMISSION_DENIED = 3,
	CLOCK_ALREADY_CORRECTED = 4,
	CHANNEL_ISNT_OPEN = 5,
	WRONG_RESULT_SIZE = 256,
	WRONG_CRC = 257,
	CHECK_CHANNEL_TIME_OUT = 258
} ResultCode;

typedef enum
{
	EXIT_OK = 0,
	EXIT_FAIL = 1
} ExitCode;

typedef enum 			// How much energy consumed: 
{
	PP_RESET = 0,		// from reset
	PP_YTD = 1,		// this year
	PP_LAST_YEAR = 2,	// last year
	PP_MONTH = 3,		// for the month specified
	PP_TODAY = 4,		// today
	PP_YESTERDAY = 5	// yesterday
} PowerPeriod;

// Compute the MODBUS RTU CRC
// Source: http://www.ccontrolsys.com/w/How_to_Compute_the_Modbus_RTU_Message_CRC
UInt16 ModRTU_CRC(byte* buf, int len)
{
  UInt16 crc = 0xFFFF;

  for (int pos = 0; pos < len; pos++) {
    crc ^= (UInt16)buf[pos];          // XOR byte into least sig. byte of crc

    for (int i = 8; i != 0; i--) {    // Loop over each bit
      if ((crc & 0x0001) != 0) {      // If the LSB is set
        crc >>= 1;                    // Shift right and XOR 0xA001
        crc ^= 0xA001;
      }
      else                            // Else LSB is not set
        crc >>= 1;                    // Just shift right
    }
  }
  // Note, this number has low and high bytes swapped, so use it accordingly (or swap bytes)
  return crc;
}


// -- Abnormal termination
void exitFailure(const char* msg)
{
	printf("%s\n\r", msg);
	exit(EXIT_FAIL);
}

// -- Print out data buffer in hex
void printPackage(byte *data, int size, int isin)
{
	if (debugPrint)
	{
		printf("%s bytes: %d\n\r\t", (isin) ? "Received" : "Sent", size);
		for (int i=0; i<size; i++)
			printf("%02X ", (byte)data[i]);
		printf("\n\r");
	}
}

// -- Non-blocking file read with timeout
// -- Returns 0 if timed out.
int nb_read_impl(int fd, byte* buf, int sz)
{
	fd_set set;
	struct timeval timeout;

	// Initialise the input set
	FD_ZERO(&set);
	FD_SET(fd, &set);

	// Set timeout
	timeout.tv_sec = CH_TIME_OUT;
	timeout.tv_usec = 0;

	int r = select(fd + 1, &set, NULL, NULL, &timeout);
	if (r < 0)
		exitFailure("Select failed.");
	else if (r == 0)
		return 0;
	else
		return read(fd, buf, BSZ);
}

// -- Non-blocking file read with timeout
// -- Aborts if timed out.
int nb_read(int fd, byte* buf, int sz)
{
	int r = nb_read_impl(fd, buf, sz);
	if (r == 0)
		exitFailure("Communication channel timeout.");
	return r;
}

// -- Check 1 byte responce
int checkResult_1b(byte* buf, int len)
{
	if (len != sizeof(Result_1b))
		return WRONG_RESULT_SIZE;

	Result_1b *res = (Result_1b*)buf;
	UInt16 crc = ModRTU_CRC(buf, len - sizeof(UInt16));
	if (crc != res->CRC)
		return WRONG_CRC;

	return res->result & 0x0F;
}

// -- Check 3 byte responce
int checkResult_3b(byte* buf, int len)
{
	if (len != sizeof(Result_3b))
		return WRONG_RESULT_SIZE;

	Result_3b *res = (Result_3b*)buf;
	UInt16 crc = ModRTU_CRC(buf, len - sizeof(UInt16));
	if (crc != res->CRC)
		return WRONG_CRC;

	return OK;
}

// -- Check 3 bytes x 3 phase responce
int checkResult_3x3b(byte* buf, int len)
{
	if (len != sizeof(Result_3x3b))
		return WRONG_RESULT_SIZE;

	Result_3x3b *res = (Result_3x3b*)buf;
	UInt16 crc = ModRTU_CRC(buf, len - sizeof(UInt16));
	if (crc != res->CRC)
		return WRONG_CRC;

	return OK;
}

// -- Check 3 bytes x 3 phase and sum responce
int checkResult_4x3b(byte* buf, int len)
{
	if (len != sizeof(Result_4x3b))
		return WRONG_RESULT_SIZE;

	Result_4x3b *res = (Result_4x3b*)buf;
	UInt16 crc = ModRTU_CRC(buf, len - sizeof(UInt16));
	if (crc != res->CRC)
		return WRONG_CRC;

	return OK;
}

// -- Check 4 bytes x 3 phase and sum responce
int checkResult_4x4b(byte* buf, int len)
{
	if (len != sizeof(Result_4x4b))
		return WRONG_RESULT_SIZE;

	Result_4x4b *res = (Result_4x4b*)buf;
	UInt16 crc = ModRTU_CRC(buf, len - sizeof(UInt16));
	if (crc != res->CRC)
		return WRONG_CRC;

	return OK;
}

// -- Check the communication channel
int checkChannel(int ttyd)
{
	// Command initialisation
	TestCmd testCmd = { .address = PM_ADDRESS, .command = 0x00 };
	testCmd.CRC = ModRTU_CRC((byte*)&testCmd, sizeof(testCmd) - sizeof(UInt16));
	printPackage((byte*)&testCmd, sizeof(testCmd), OUT);

	// Send test channel command
	write(ttyd, (byte*)&testCmd, sizeof(testCmd));
	usleep(TIME_OUT);

	// Get responce
	byte buf[BSZ];
	int len = nb_read_impl(ttyd, buf, BSZ);
	if (len == 0)
		return CHECK_CHANNEL_TIME_OUT;

	printPackage((byte*)buf, len, IN);

	return checkResult_1b(buf, len);
}

// -- Connection initialisation
int initConnection(int ttyd)
{
	InitCmd initCmd = {
		.address = PM_ADDRESS,
		.command = 0x01,
		.accessLevel = 0x01,
		.password = { 0x01, 0x01, 0x01, 0x01, 0x01, 0x01 },
	};
	initCmd.CRC = ModRTU_CRC((byte*)&initCmd, sizeof(initCmd) - sizeof(UInt16));
	printPackage((byte*)&initCmd, sizeof(initCmd), OUT);

	write(ttyd, (byte*)&initCmd, sizeof(initCmd));
	usleep(TIME_OUT);

	// Read initialisation result
	byte buf[BSZ];
	int len = nb_read(ttyd, buf, BSZ);
	printPackage((byte*)buf, len, IN);

	return checkResult_1b(buf, len);
}

// -- Close connection
int closeConnection(int ttyd)
{
	ByeCmd byeCmd = { .address = PM_ADDRESS, .command = 0x02 };
	byeCmd.CRC = ModRTU_CRC((byte*)&byeCmd, sizeof(byeCmd) - sizeof(UInt16));
	printPackage((byte*)&byeCmd, sizeof(byeCmd), OUT);

	write(ttyd, (byte*)&byeCmd, sizeof(byeCmd));
	usleep(TIME_OUT);

	// Read closing responce
	byte buf[BSZ];
	int len = nb_read(ttyd, buf, BSZ);
	printPackage((byte*)buf, len, IN);

	return checkResult_1b(buf, len);
}

// Decode float from 3 bytes
float B3F(byte b[3], float factor)
{
	int val = (b[0] << 16) | (b[2] << 8) | b[1];
	return val/factor;
}

// Decode float from 4 bytes
float B4F(byte b[4], float factor)
{
	int val = (b[1] << 24) | (b[0] << 16) | (b[3] << 8) | b[2];
	return val/factor;
}

// Get voltage (U) by phases
int getU(int ttyd, P3V* U)
{
	ReadParamCmd getUCmd =
	{
		.address = PM_ADDRESS,
		.command = 0x08,
		.paramId = 0x16,
		.BWRI = 0x11
	};
	getUCmd.CRC = ModRTU_CRC((byte*)&getUCmd, sizeof(getUCmd) - sizeof(UInt16));
	printPackage((byte*)&getUCmd, sizeof(getUCmd), OUT);

	write(ttyd, (byte*)&getUCmd, sizeof(getUCmd));
	usleep(TIME_OUT);

	// Read responce
	byte buf[BSZ];
	int len = nb_read(ttyd, buf, BSZ);
	printPackage((byte*)buf, len, IN);

	// Check and decode result
	int checkResult = checkResult_3x3b(buf, len);
	if (OK == checkResult)
	{
		Result_3x3b* res = (Result_3x3b*)buf;
		U->p1 = B3F(res->p1, 100.0);
		U->p2 = B3F(res->p2, 100.0);
		U->p3 = B3F(res->p3, 100.0);
	}

	return checkResult;
}

// Get current (I) by phases
int getI(int ttyd, P3V* I)
{
	ReadParamCmd getICmd =
	{
		.address = PM_ADDRESS,
		.command = 0x08,
		.paramId = 0x16,
		.BWRI = 0x21
	};
	getICmd.CRC = ModRTU_CRC((byte*)&getICmd, sizeof(getICmd) - sizeof(UInt16));
	printPackage((byte*)&getICmd, sizeof(getICmd), OUT);

	write(ttyd, (byte*)&getICmd, sizeof(getICmd));
	usleep(TIME_OUT);

	// Read responce
	byte buf[BSZ];
	int len = nb_read(ttyd, buf, BSZ);
	printPackage((byte*)buf, len, IN);

	// Check and decode result
	int checkResult = checkResult_3x3b(buf, len);
	if (OK == checkResult)
	{
		Result_3x3b* res = (Result_3x3b*)buf;
		I->p1 = B3F(res->p1, 1000.0);
		I->p2 = B3F(res->p2, 1000.0);
		I->p3 = B3F(res->p3, 1000.0);
	}

	return checkResult;
}

// Get power consumption factor cos(f) by phases
int getCosF(int ttyd, P3VS* C)
{
	ReadParamCmd getCosCmd =
	{
		.address = PM_ADDRESS,
		.command = 0x08,
		.paramId = 0x16,
		.BWRI = 0x30
	};
	getCosCmd.CRC = ModRTU_CRC((byte*)&getCosCmd, sizeof(getCosCmd) - sizeof(UInt16));
	printPackage((byte*)&getCosCmd, sizeof(getCosCmd), OUT);

	write(ttyd, (byte*)&getCosCmd, sizeof(getCosCmd));
	usleep(TIME_OUT);

	// Read responce
	byte buf[BSZ];
	int len = nb_read(ttyd, buf, BSZ);
	printPackage((byte*)buf, len, IN);

	// Check and decode result
	int checkResult = checkResult_4x3b(buf, len);
	if (OK == checkResult)
	{
		Result_4x3b* res = (Result_4x3b*)buf;
		C->p1 = B3F(res->p1, 1000.0);
		C->p2 = B3F(res->p2, 1000.0);
		C->p3 = B3F(res->p3, 1000.0);
		C->sum = B3F(res->sum, 1000.0);
	}

	return checkResult;
}

// Get grid frequency (Hz)
int getF(int ttyd, float *f)
{
	ReadParamCmd getFCmd =
	{
		.address = PM_ADDRESS,
		.command = 0x08,
		.paramId = 0x16,
		.BWRI = 0x40
	};
	getFCmd.CRC = ModRTU_CRC((byte*)&getFCmd, sizeof(getFCmd) - sizeof(UInt16));
	printPackage((byte*)&getFCmd, sizeof(getFCmd), OUT);

	write(ttyd, (byte*)&getFCmd, sizeof(getFCmd));
	usleep(TIME_OUT);

	// Read responce
	byte buf[BSZ];
	int len = nb_read(ttyd, buf, BSZ);
	printPackage((byte*)buf, len, IN);

	// Check and decode result
	int checkResult = checkResult_3b(buf, len);
	if (OK == checkResult)
	{
		Result_3b* res = (Result_3b*)buf;
		*f = B3F(res->res, 100.0);
	}

	return checkResult;
}

// Get phases angle
int getA(int ttyd, P3V* A)
{
	ReadParamCmd getACmd =
	{
		.address = PM_ADDRESS,
		.command = 0x08,
		.paramId = 0x16,
		.BWRI = 0x51
	};
	getACmd.CRC = ModRTU_CRC((byte*)&getACmd, sizeof(getACmd) - sizeof(UInt16));
	printPackage((byte*)&getACmd, sizeof(getACmd), OUT);

	write(ttyd, (byte*)&getACmd, sizeof(getACmd));
	usleep(TIME_OUT);

	// Read responce
	byte buf[BSZ];
	int len = nb_read(ttyd, buf, BSZ);
	printPackage((byte*)buf, len, IN);

	// Check and decode result
	int checkResult = checkResult_3x3b(buf, len);
	if (OK == checkResult)
	{
		Result_3x3b* res = (Result_3x3b*)buf;
		A->p1 = B3F(res->p1, 100.0);
		A->p2 = B3F(res->p2, 100.0);
		A->p3 = B3F(res->p3, 100.0);
	}

	return checkResult;
}

// Get active power (W) consumption by phases with total
int getP(int ttyd, P3VS* P)
{
	ReadParamCmd getPCmd =
	{
		.address = PM_ADDRESS,
		.command = 0x08,
		.paramId = 0x16,
		.BWRI = 0x00
	};
	getPCmd.CRC = ModRTU_CRC((byte*)&getPCmd, sizeof(getPCmd) - sizeof(UInt16));
	printPackage((byte*)&getPCmd, sizeof(getPCmd), OUT);

	write(ttyd, (byte*)&getPCmd, sizeof(getPCmd));
	usleep(TIME_OUT);

	// Read responce
	byte buf[BSZ];
	int len = nb_read(ttyd, buf, BSZ);
	printPackage((byte*)buf, len, IN);

	// Check and decode result
	int checkResult = checkResult_4x3b(buf, len);
	if (OK == checkResult)
	{
		Result_4x3b* res = (Result_4x3b*)buf;
		P->p1 = B3F(res->p1, 1000.0);
		P->p2 = B3F(res->p2, 1000.0);
		P->p3 = B3F(res->p3, 1000.0);
		P->sum = B3F(res->sum, 1000.0);
	}

	return checkResult;
}

// Get reactive power (VA) consumption by phases with total
int getS(int ttyd, P3VS* S)
{
	ReadParamCmd getSCmd =
	{
		.address = PM_ADDRESS,
		.command = 0x08,
		.paramId = 0x16,
		.BWRI = 0x08
	};
	getSCmd.CRC = ModRTU_CRC((byte*)&getSCmd, sizeof(getSCmd) - sizeof(UInt16));
	printPackage((byte*)&getSCmd, sizeof(getSCmd), OUT);

	write(ttyd, (byte*)&getSCmd, sizeof(getSCmd));
	usleep(TIME_OUT);

	// Read responce
	byte buf[BSZ];
	int len = nb_read(ttyd, buf, BSZ);
	printPackage((byte*)buf, len, IN);

	// Check and decode result
	int checkResult = checkResult_4x3b(buf, len);
	if (OK == checkResult)
	{
		Result_4x3b* res = (Result_4x3b*)buf;
		S->p1 = B3F(res->p1, 1000.0);
		S->p2 = B3F(res->p2, 1000.0);
		S->p3 = B3F(res->p3, 1000.0);
		S->sum = B3F(res->sum, 1000.0);
	}

	return checkResult;
}

/* Get power counters by phases for the period
	periodId - one of PowerPeriod enum values
	month - month number when periodId is PP_MONTH
	tariffNo - 0 for all tariffs, 1 - tariff #1, 2 - tariff #2 etc.  */
int getW(int ttyd, P3VS* W, int periodId, int month, int tariffNo)
{
	ReadParamCmd getWCmd =
	{
		.address = PM_ADDRESS,
		.command = 0x05,
		.paramId = (periodId << 4) | (month & 0xF),
		.BWRI = tariffNo
	};
	getWCmd.CRC = ModRTU_CRC((byte*)&getWCmd, sizeof(getWCmd) - sizeof(UInt16));
	printPackage((byte*)&getWCmd, sizeof(getWCmd), OUT);

	write(ttyd, (byte*)&getWCmd, sizeof(getWCmd));
	usleep(TIME_OUT);

	// Read responce
	byte buf[BSZ];
	int len = nb_read(ttyd, buf, BSZ);
	printPackage((byte*)buf, len, IN);

	// Check and decode result
	int checkResult = checkResult_4x4b(buf, len);
	if (OK == checkResult)
	{
		Result_4x4b* res = (Result_4x4b*)buf;
		W->p1 = B4F(res->p1, 1000.0);
		W->p2 = B4F(res->p2, 1000.0);
		W->p3 = B4F(res->p3, 1000.0);
		W->sum = B4F(res->sum, 1000.0);
	}

	return checkResult;
}

void printUsage()
{
	printf("Usage: mercury236 [OPTIONS] ...\n\r\n\r");
	printf("\t--debug\t\tto print extra debug info\n\r", OPT_DEBUG);
	printf("\t--help\t\tprints this screen\n\r", OPT_HELP);
}

int main(int argc, const char** args)
{
	int fd;
	struct termios oldtio, newtio;

	for (int i=1; i<argc; i++)
	{
		if (!strcmp(OPT_DEBUG, args[i]))
			debugPrint = 1;
		else
		{
			printUsage();
			exit(EXIT_OK);
		}
	}

	// Open RS485 dongle
	fd = open(MODEMDEVICE, O_RDWR | O_NOCTTY | O_NDELAY );
	if (fd < 0)
	{
		perror(MODEMDEVICE);
		exit(EXIT_FAIL);
	}
	fcntl(fd, F_SETFL, 0);

	tcgetattr(fd, &oldtio); /* save current port settings */

	bzero(&newtio, sizeof(newtio));

	cfsetispeed(&newtio, BAUDRATE);
	cfsetospeed(&newtio, BAUDRATE);

	newtio.c_cflag = BAUDRATE | CS8 | CLOCAL | CREAD;
//	newtio.c_cflag = BAUDRATE | CRTSCTS | CS8 | CLOCAL | CREAD;
//	newtio.c_cflag = BAUDRATE | CS8 | CREAD;
	newtio.c_iflag = IGNPAR;
	newtio.c_oflag = 0;

	cfmakeraw(&newtio);
	tcsetattr(fd, TCSANOW, &newtio);

	P3V U, I, A;
	P3VS C, P, S, PR, PY, PT;
	float f;

	switch(checkChannel(fd))
	{
		case OK:
			if (OK != checkChannel(fd))
				exitFailure("Power meter communication channel test failed.");

			if (OK != initConnection(fd))
				exitFailure("Power meter connection initialisation error.");

			// Get voltage by phases
			if (OK != getU(fd, &U))
				exitFailure("Cannot collect voltage data.");

			// Get current by phases
			if (OK != getI(fd, &I))
				exitFailure("Cannot collect current data.");

			// Get power cos(f) by phases
			if (OK != getCosF(fd, &C))
				exitFailure("Cannot collect cos(f) data.");

			// Get grid frequency
			if (OK != getF(fd, &f))
				exitFailure("Cannot collect grid frequency data.");

			// Get phase angles
			if (OK != getA(fd, &A))
				exitFailure("Cannot collect phase angles data.");

			// Get active power consumption by phases
			if (OK != getP(fd, &P))
				exitFailure("Cannot collect active power consumption data.");

			// Get reactive power consumption by phases
			if (OK != getS(fd, &S))
				exitFailure("Cannot collect reactive power consumption data.");

			// Get power counter from reset, for yesterday and today
			if (OK != getW(fd, &PR, PP_RESET, 0, 0) ||
			    OK != getW(fd, &PY, PP_YESTERDAY, 0, 0) ||
			    OK != getW(fd, &PT, PP_TODAY, 0, 0))
				exitFailure("Cannot collect power counters data.");

			if (OK != closeConnection(fd))
				exitFailure("Power meter connection closing error.");

			break;

		case CHECK_CHANNEL_TIME_OUT:
			U.p1 = U.p2 = U.p3 = 0.0;
			I.p1 = I.p2 = I.p3 = 0.0;
			P.p1 = P.p2 = P.p3 = P.sum = 0.0;
			S.p1 = S.p2 = S.p3 = S.sum = 0.0;
			break;

		default:
			exitFailure("Check channel failure.");
	}

	close(fd);
	tcsetattr(fd, TCSANOW, &oldtio);

	printf("U (V):   %8.2f %8.2f %8.2f\n\r", U.p1, U.p2, U.p3);
	printf("I (A):   %8.2f %8.2f %8.2f\n\r", I.p1, I.p2, I.p3);
	printf("Cos(f):  %8.2f %8.2f %8.2f (%8.2f)\n\r", C.p1, C.p2, C.p3, C.sum);
	printf("F (Hz):  %8.2f\n\r", f);
	printf("A (deg): %8.2f %8.2f %8.2f\n\r", A.p1, A.p2, A.p3);
	printf("P (W):   %8.2f %8.2f %8.2f (%8.2f)\n\r", P.p1, P.p2, P.p3, P.sum);
	printf("S (VA):  %8.2f %8.2f %8.2f (%8.2f)\n\r", S.p1, S.p2, S.p3, S.sum);
	printf("PR (KW): %8.2f %8.2f %8.2f (%8.2f)\n\r", PR.p1, PR.p2, PR.p3, PR.sum);
	printf("PY (KW): %8.2f %8.2f %8.2f (%8.2f)\n\r", PY.p1, PY.p2, PY.p3, PY.sum);
	printf("PT (KW): %8.2f %8.2f %8.2f (%8.2f)\n\r", PT.p1, PT.p2, PT.p3, PT.sum);

	exit(EXIT_OK);
}

