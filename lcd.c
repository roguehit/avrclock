/*
 ** ----------------------------------------------------------------------------
 ** "THE BEER-WARE LICENSE" (Revision 42):
 ** <roguehit@gmail.com> wrote this file. As long as you retain this notice you
 ** can do whatever you want with this stuff. If we meet some day, and you think
 ** this stuff is worth it, you can buy me a beer in return Rohit N.
 ** ----------------------------------------------------------------------------
 ** LCD stuff comes from avr-libc/docs/examples/twistdio/
 ** i2c stuff comes from http://www.avrfreaks.net/index.php?module=Freaks%20Academy&func=viewItem&item_id=1754&item_type=project
 **/

#include "defines.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include <avr/io.h>
#include <util/delay.h>

#include <inttypes.h>
#include <avr/interrupt.h>
#include <avr/sleep.h>

#include <stdlib.h>

#include <util/delay.h>
#include <util/twi.h>

#include "common.h"

#define	 START			0x08
#define  REPEAT_START		0x10
#define  MT_SLA_ACK		0x18
#define  MT_SLA_NACK		0x20
#define  MT_DATA_ACK		0x28
#define  MT_DATA_NACK		0x30
#define  MR_SLA_ACK		0x40
#define  MR_SLA_NACK		0x48
#define  MR_DATA_ACK		0x50
#define  MR_DATA_NACK		0x58
#define  ARB_LOST		0x38
#define  ERROR_CODE		0x7e
#define  DS1307_W		0xd0
#define  DS1307_R		0xd1
#define	 SECONDS		rtc_register[0]
#define	 MINUTES		rtc_register[1]
#define	 HOURS			rtc_register[2]
#define	 DAY    		rtc_register[3]
#define	 DATE   		rtc_register[4]
#define	 MONTH  		rtc_register[5]
#define	 YEAR   		rtc_register[6]

#define GLUE(a, b)     a##b
#define HD44780_BUSYFLAG 0x80


/* single-bit macros, used for control bits */
#define SET_(what, p, m) GLUE(what, p) |= (1 << (m))
#define CLR_(what, p, m) GLUE(what, p) &= ~(1 << (m))
#define GET_(/* PIN, */ p, m) GLUE(PIN, p) & (1 << (m))
#define SET(what, x) SET_(what, x)
#define CLR(what, x) CLR_(what, x)
#define GET(/* PIN, */ x) GET_(x)

/* nibble macros, used for data path */
#define ASSIGN_(what, p, m, v) GLUE(what, p) = (GLUE(what, p) & \
      ~((1 << (m)) | (1 << ((m) + 1)) | \
	 (1 << ((m) + 2)) | (1 << ((m) + 3)))) | \
((v) << (m))
#define READ_(what, p, m) (GLUE(what, p) & ((1 << (m)) | (1 << ((m) + 1)) | \
	 (1 << ((m) + 2)) | (1 << ((m) + 3)))) >> (m)
#define ASSIGN(what, x, v) ASSIGN_(what, x, v)
#define READ(what, x) READ_(what, x)


unsigned char i2c_start(void);
unsigned char i2c_repeatStart(void);
unsigned char i2c_sendAddress(unsigned char);
unsigned char i2c_sendData(unsigned char);
unsigned char i2c_receiveData_ACK(void);
unsigned char i2c_receiveData_NACK(void);
void i2c_stop(void);

FILE lcd_str = FDEV_SETUP_STREAM(lcd_putchar, NULL, _FDEV_SETUP_WRITE);

char rtc_register[7];
char time[10]; 		//xxam:xx:xx;
char date[12];		//xx/xx/xxxx;
char day;

/*
 * Send one pulse to the E signal (enable).  Mind the timing
 * constraints.  If readback is set to true, read the HD44780 data
 * pins right before the falling edge of E, and return that value.
 */
static inline uint8_t
hd44780_pulse_e(bool readback) __attribute__((always_inline));

   static inline uint8_t
hd44780_pulse_e(bool readback)
{
   uint8_t x;

   SET(PORT, HD44780_E);
   /*
    * Guarantee at least 500 ns of pulse width.  For high CPU
    * frequencies, a delay loop is used.  For lower frequencies, NOPs
    * are used, and at or below 1 MHz, the native pulse width will
    * already be 1 us or more so no additional delays are needed.
    */
#if F_CPU > 4000000UL
   _delay_us(0.5);
#else
   /*
    * When reading back, we need one additional NOP, as the value read
    * back from the input pin is sampled close to the beginning of a
    * CPU clock cycle, while the previous edge on the output pin is
    * generated towards the end of a CPU clock cycle.
    */
   if (readback)
      __asm__ volatile("nop");
#  if F_CPU > 1000000UL
   __asm__ volatile("nop");
#    if F_CPU > 2000000UL
   __asm__ volatile("nop");
   __asm__ volatile("nop");
#    endif /* F_CPU > 2000000UL */
#  endif /* F_CPU > 1000000UL */
#endif
   if (readback)
      x = READ(PIN, HD44780_D4);
   else
      x = 0;
   CLR(PORT, HD44780_E);

   return x;
}

/*
 * Send one nibble out to the LCD controller.
 */
   static void
hd44780_outnibble(uint8_t n, uint8_t rs)
{
   CLR(PORT, HD44780_RW);
   if (rs)
      SET(PORT, HD44780_RS);
   else
      CLR(PORT, HD44780_RS);
   ASSIGN(PORT, HD44780_D4, n);
   (void)hd44780_pulse_e(false);
}

/*
 * Send one byte to the LCD controller.  As we are in 4-bit mode, we
 * have to send two nibbles.
 */
   void
hd44780_outbyte(uint8_t b, uint8_t rs)
{
   hd44780_outnibble(b >> 4, rs);
   hd44780_outnibble(b & 0xf, rs);
}

/*
 * Read one nibble from the LCD controller.
 */
   static uint8_t
hd44780_innibble(uint8_t rs)
{
   uint8_t x;

   SET(PORT, HD44780_RW);
   ASSIGN(DDR, HD44780_D4, 0x00);
   if (rs)
      SET(PORT, HD44780_RS);
   else
      CLR(PORT, HD44780_RS);
   x = hd44780_pulse_e(true);
   ASSIGN(DDR, HD44780_D4, 0x0F);
   CLR(PORT, HD44780_RW);

   return x;
}

/*
 * Read one byte (i.e. two nibbles) from the LCD controller.
 */
   uint8_t
hd44780_inbyte(uint8_t rs)
{
   uint8_t x;

   x = hd44780_innibble(rs) << 4;
   x |= hd44780_innibble(rs);

   return x;
}

/*
 * Wait until the busy flag is cleared.
 */
   void
hd44780_wait_ready(bool longwait)
{
#if USE_BUSY_BIT
   while (hd44780_incmd() & HD44780_BUSYFLAG) ;
#else
   if (longwait)
      _delay_ms(1.52);
   else
      _delay_us(37);
#endif
}

/*
 * Initialize the LCD controller.
 *
 * The initialization sequence has a mandatory timing so the
 * controller can safely recognize the type of interface desired.
 * This is the only area where timed waits are really needed as
 * the busy flag cannot be probed initially.
 */
   void
hd44780_init(void)
{
   SET(DDR, HD44780_RS);
   SET(DDR, HD44780_RW);
   SET(DDR, HD44780_E);
   ASSIGN(DDR, HD44780_D4, 0x0F);

   _delay_ms(15);		/* 40 ms needed for Vcc = 2.7 V */
   hd44780_outnibble(HD44780_FNSET(1, 0, 0) >> 4, 0);
   _delay_ms(4.1);
   hd44780_outnibble(HD44780_FNSET(1, 0, 0) >> 4, 0);
   _delay_ms(0.1);
   hd44780_outnibble(HD44780_FNSET(1, 0, 0) >> 4, 0);
   _delay_us(37);

   hd44780_outnibble(HD44780_FNSET(0, 1, 0) >> 4, 0);
   hd44780_wait_ready(false);
   hd44780_outcmd(HD44780_FNSET(0, 1, 0));
   hd44780_wait_ready(false);
   hd44780_outcmd(HD44780_DISPCTL(0, 0, 0));
   hd44780_wait_ready(false);
}

/*
 * Prepare the LCD controller pins for powerdown.
 */
   void
hd44780_powerdown(void)
{
   ASSIGN(PORT, HD44780_D4, 0);
   CLR(PORT, HD44780_RS);
   CLR(PORT, HD44780_RW);
   CLR(PORT, HD44780_E);
}

/*
 * Setup the LCD controller.  First, call the hardware initialization
 * function, then adjust the display attributes we want.
 */
   void
lcd_init(void)
{

   hd44780_init();

   /*
    * Clear the display.
    */
   hd44780_outcmd(HD44780_CLR);
   hd44780_wait_ready(0);

   /*
    * Entry mode: auto-increment address counter, no display shift in
    * effect.
    */
   hd44780_outcmd(HD44780_ENTMODE(1, 0));
   hd44780_wait_ready(0);

   /*
    * Enable display, activate non-blinking cursor.
    */
   hd44780_outcmd(HD44780_DISPCTL(1, 1, 0));
   hd44780_wait_ready(0);
}

/*
 * Send character c to the LCD display.  After a '\n' has been seen,
 * the next character will first clear the display.
 */
   int
lcd_putchar(char c, FILE *unused)
{
   static bool nl_seen;

   if (nl_seen && c != '\n')
   {
      /*
       * First character after newline, clear display and home cursor.
       */
      hd44780_wait_ready(0);
      hd44780_outcmd(HD44780_CLR);
      hd44780_wait_ready(0);
      hd44780_outcmd(HD44780_HOME);
      hd44780_wait_ready(0);
      hd44780_outcmd(HD44780_DDADDR(0));

      nl_seen = false;
   }
   if (c == '\n')
   {
      nl_seen = true;
   }
   else
   {
      hd44780_wait_ready(0);
      hd44780_outdata(c);
   }

   return 0;
}

//*************************************************
//Function to start i2c communication
//*************************************************
unsigned char i2c_start(void)
{

   TWCR = (1<<TWINT)|(1<<TWSTA)|(1<<TWEN); 	//Send START condition

   while (!(TWCR & (1<<TWINT)));   		//Wait for TWINT flag set. This indicates that the
   //START condition has been transmitted
   if ((TWSR & 0xF8) == START)			//Check value of TWI Status Register
      return(0);
   else
      return(1);
}

//*************************************************
//Function for repeat start condition
//*************************************************
unsigned char i2c_repeatStart(void)
{

   TWCR = (1<<TWINT)|(1<<TWSTA)|(1<<TWEN); 		//Send START condition
   while (!(TWCR & (1<<TWINT)));   		//Wait for TWINT flag set. This indicates that the
   //START condition has been transmitted
   if ((TWSR & 0xF8) == REPEAT_START)			//Check value of TWI Status Register
      return(0);
   else
      return(1);
}
//**************************************************
//Function to transmit address of the slave
//*************************************************
unsigned char i2c_sendAddress(unsigned char address)
{
   unsigned char STATUS;

   if((address & 0x01) == 0) 
      STATUS = MT_SLA_ACK;
   else
      STATUS = MR_SLA_ACK; 

   TWDR = address; 
   TWCR = (1<<TWINT)|(1<<TWEN);	   //Load SLA_W into TWDR Register. Clear TWINT bit
   //in TWCR to start transmission of address
   while (!(TWCR & (1<<TWINT)));	   //Wait for TWINT flag set. This indicates that the
   //SLA+W has been transmitted, and
   //ACK/NACK has been received.
   if ((TWSR & 0xF8) == STATUS)	   //Check value of TWI Status Register
      return(0);
   else 
      return(1);
}

//**************************************************
//Function to transmit a data byte
//*************************************************
unsigned char i2c_sendData(unsigned char data)
{
   TWDR = data; 
   TWCR = (1<<TWINT) |(1<<TWEN);	   //Load SLA_W into TWDR Register. Clear TWINT bit
   //in TWCR to start transmission of data
   while (!(TWCR & (1<<TWINT)));	   //Wait for TWINT flag set. This indicates that the
   //data has been transmitted, and
   //ACK/NACK has been received.
   if ((TWSR & 0xF8) != MT_DATA_ACK)   //Check value of TWI Status Register
      return(1);
   else
      return(0);
}

//*****************************************************
//Function to receive a data byte and send ACKnowledge
//*****************************************************
unsigned char i2c_receiveData_ACK(void)
{
   unsigned char data;

   TWCR = (1<<TWEA)|(1<<TWINT)|(1<<TWEN);

   while (!(TWCR & (1<<TWINT)));	   	   //Wait for TWINT flag set. This indicates that the
   //data has been received
   if ((TWSR & 0xF8) != MR_DATA_ACK)    //Check value of TWI Status Register
      return(ERROR_CODE);

   data = TWDR;
   return(data);
}

//******************************************************************
//Function to receive the last data byte (no acknowledge from master
//******************************************************************
unsigned char i2c_receiveData_NACK(void)
{
   unsigned char data;

   TWCR = (1<<TWINT)|(1<<TWEN);

   while (!(TWCR & (1<<TWINT)));	   	   //Wait for TWINT flag set. This indicates that the
   //data has been received
   if ((TWSR & 0xF8) != MR_DATA_NACK)    //Check value of TWI Status Register
      return(ERROR_CODE);

   data = TWDR;
   return(data);
}
//**************************************************
//Function to end the i2c communication
//*************************************************   	
void i2c_stop(void)
{
   TWCR =  (1<<TWINT)|(1<<TWEN)|(1<<TWSTO);	  //Transmit STOP condition
}  


//***************************************************************************
//Function to set initial address of the RTC for subsequent reading / writing
//***************************************************************************
void RTC_setStartAddress(void)
{
   unsigned char errorStatus;

   errorStatus = i2c_start();
   if(errorStatus == 1)
   {
      fprintf(stderr,"RTC start1 failed..\n");
      i2c_stop();
      return;
   } 

   errorStatus = i2c_sendAddress(DS1307_W);

   if(errorStatus == 1)
   {
      fprintf(stderr,"RTC sendAddress1 failed..\n");
      i2c_stop();
      return;
   } 

   errorStatus = i2c_sendData(0x00);
   if(errorStatus == 1)
   {
      fprintf(stderr,"RTC write-2 failed..\n");
      i2c_stop();
      return;
   } 

   i2c_stop();
}

//***********************************************************************
//Function to read RTC registers and store them in buffer rtc_register[]
//***********************************************************************    
void RTC_read(void)
{

   unsigned char errorStatus, i, data;

   errorStatus = i2c_start();
   if(errorStatus == 1)
   {
      fprintf(stderr,"RTC start1 failed..\n");
      i2c_stop();
      return;
   } 

   errorStatus = i2c_sendAddress(DS1307_W);

   if(errorStatus == 1)
   {
      fprintf(stderr,"RTC sendAddress1 failed..\n");
      i2c_stop();
      return;
   } 

   errorStatus = i2c_sendData(0x00);
   if(errorStatus == 1)
   {
      fprintf(stderr,"RTC write-1 failed..\n");
      i2c_stop();
      return;
   } 

   errorStatus = i2c_repeatStart();
   if(errorStatus == 1)
   {
      fprintf(stderr,"RTC repeat start failed..\n");
      i2c_stop();
      return;
   } 

   errorStatus = i2c_sendAddress(DS1307_R);

   if(errorStatus == 1)
   {
      fprintf(stderr,"RTC sendAddress2 failed..\n");
      i2c_stop();
      return;
   } 

   for(i=0;i<7;i++)
   {
      if(i == 6)  	 //no Acknowledge after receiving the last byte
	 data = i2c_receiveData_NACK();
      else
	 data = i2c_receiveData_ACK();

      if(data == ERROR_CODE)
      {
	 fprintf(stderr,"RTC receive failed..\n");
	 i2c_stop();
	 return;
      }

      rtc_register[i] = data;
   }

   i2c_stop();
}	  

//******************************************************************
//Function to form time string for sending it to LCD & UART
//****************************************************************** 
void RTC_getTime(void)
{
   RTC_read();
   time[8] = 0x00;	  //NIL
   time[7] = (SECONDS & 0x0f) | 0x30;		//seconds(1's)
   time[6] = ((SECONDS & 0x70) >> 4) | 0x30;		//seconds(10's)
   time[5] = ':';

   time[4] = (MINUTES & 0x0f) | 0x30;
   time[3] = ((MINUTES & 0x70) >> 4) | 0x30;
   time[2] = ':'; 

   time[1] = (HOURS & 0x0f) | 0x30;	
   time[0] = ((HOURS & 0x30) >> 4) | 0x30;
}
void RTC_getDate(void)
{
   RTC_read();
   date[11] = 0x00;
   date[10] = 0x00;
   date[9] = (YEAR & 0x0f) | 0x30;
   date[8] = ((YEAR & 0xf0) >> 4) | 0x30;
   date[7] = '0';
   date[6] = '2';
   date[5] = '/';
   date[4] = (MONTH & 0x0f) | 0x30;
   date[3] = ((MONTH & 0x10) >> 4) | 0x30;
   date[2] = '/';
   date[1] = (DATE & 0x0f) | 0x30;
   date[0] = ((DATE & 0x30) >> 4) | 0x30;
}  


//TWI initialize
// bit rate:18 (freq: 100Khz)
void twi_init(void)
{
   TWCR= 0X00; //disable twi
   TWBR= 0x12; //set bit rate
   TWSR= 0x01; //set prescale
   TWAR= 0x00; //set slave address
   TWCR= 0x44; //enable twi
}

//******************************************************************
//Function to update buffer rtc_register[] for next writing to RTC
//****************************************************************** 
void RTC_updateRegisters(void)
{
   SECONDS = ((time[6] & 0x07) << 4) | (time[7] & 0x0f);
   MINUTES = ((time[3] & 0x07) << 4) | (time[4] & 0x0f);
   HOURS = ((time[0] & 0x03) << 4) | (time[1] & 0x0f);  
   DAY = date[10];
   DATE = ((date[3] & 0x03) << 4) | (date[4] & 0x0f);
   MONTH = ((date[0] & 0x01) << 4) | (date[1] & 0x0f);
   YEAR = ((date[8] & 0x0f) << 4) | (date[9] & 0x0f);
}  
//******************************************************************
//Function to write new time in the RTC 
//******************************************************************   
unsigned char RTC_writeTime(void)
{
   unsigned char errorStatus, i;

   errorStatus = i2c_start();
   if(errorStatus == 1)
   {
      fprintf(stderr,"RTC start1 failed..\n");
      i2c_stop();
      return(1);
   } 

   errorStatus = i2c_sendAddress(DS1307_W);

   if(errorStatus == 1)
   {
      fprintf(stderr,"RTC sendAddress1 failed..\n");
      i2c_stop();
      return(1);
   } 

   errorStatus = i2c_sendData(0x00);
   if(errorStatus == 1)
   {
      fprintf(stderr,"RTC write-1 failed..\n");
      i2c_stop();
      return(1);
   } 

   for(i=0;i<3;i++)
   {
      errorStatus = i2c_sendData(rtc_register[i]);  
      if(errorStatus == 1)
      {
	 fprintf(stderr,"RTC write time failed..\n");
	 i2c_stop();
	 return(1);
      }
   }

   i2c_stop();
   return(0);
}

void RTC_updateTime()
{
   unsigned char data = 0;


   time[0] = 1; //18:20:38  
   time[1] = 8;
   
   time[3] = 2;
   time[4] = 0;

   time[6] = 3;
   time[7] = 8;

   RTC_updateRegisters(); 
   data = RTC_writeTime();

   if(data == 0)
      fprintf(stderr,"Time Updated sucessfully..\n"); 
   else
      fprintf(stderr,"Invalid Entry..\n"); 

   return;
}

//******************************************************************
//Function to write new date in the RTC
//******************************************************************   
unsigned char RTC_writeDate(void)
{
   unsigned char errorStatus, i;

   errorStatus = i2c_start();
   if(errorStatus == 1)
   {
      fprintf(stderr,"RTC start1 failed..\n");
      i2c_stop();
      return(1);
   } 

   errorStatus = i2c_sendAddress(DS1307_W);

   if(errorStatus == 1)
   {
      fprintf(stderr,"RTC sendAddress1 failed..\n");
      i2c_stop();
      return(1);
   } 

   errorStatus = i2c_sendData(0x03);
   if(errorStatus == 1)
   {
      fprintf(stderr,"RTC write-1 failed..\n");
      i2c_stop();
      return(1);
   } 

   for(i=3;i<7;i++)
   {
      errorStatus = i2c_sendData(rtc_register[i]);  
      if(errorStatus == 1)
      {
	 fprintf(stderr,"RTC write date failed..\n");
	 i2c_stop();
	 return(1);
      }
   }

   i2c_stop();
   return(0);
}

void RTC_updateDate(void)
{
   unsigned char data = 0;

   date[0] = 0;//mm/dd/yy - 03/06/11
   date[1] = 3;
   
   date[3] = 0;
   date[4] = 6;
   
   date[8] = 1;
   date[9] = 1;

   RTC_updateRegisters(); 
   data = RTC_writeDate();

   if(data == 0)
      fprintf(stderr,"Date Updated sucessfully..\n"); 
   else
   fprintf(stderr,"Invalid Entry..\n"); 

   return;
}  

void main()
{
   int i = 0;
   lcd_init();
   twi_init();

   stderr = &lcd_str;

   fprintf(stderr,"Read...\n");
   //	RTC_updateTime();
   //	RTC_updateDate();
   while(1)
   {	
      RTC_getTime();	
      RTC_getDate();
      fprintf(stderr,"Time:");

      for(i = 0 ; i < 8 ; i++)
	 fprintf(stderr,"%c",time[i]);

      //FIXME This is required so that it rolls to next line
      fprintf(stderr,"                           ");

      fprintf(stderr,"Date:");
      fprintf(stderr,"%c%c/%c%c/20%c%c",date[3],date[4],date[0],date[1],date[8],date[9]);
      fprintf(stderr,"\n");
      _delay_ms(1000);
   }

   return;
}
