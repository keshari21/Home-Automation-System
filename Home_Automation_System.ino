/* IRRemote library
 * Interrupt based IR receiver library
 * Data decoded according to NEC protocol
 *
 * Version 1.0, October 2015 
 * Created by Jelle Roets (Circuits.io)
 * --------------------------------------
*/

#define IRRECEIVER_BUFFERSIZE 16
#define IRRECEIVER_STATE_IDLE 0
#define IRRECEIVER_STATE_START 1
#define IRRECEIVER_STATE_BITS 2
#define IRRECEIVER_STATE_REPEAT 3

typedef struct 
{
  unsigned int address;
  unsigned int command;
  byte rawBytes[4];
  bool repeat;
  bool error;
} IRdata;

class IRreceiver 
{
  public:
    IRreceiver(int pin);	// Constructor: bind a new IR receiver object to a pin (enable pin change interrupts)
    ~IRreceiver();			// Destructor: release IR receiver (disables pin change interrupts)
    
    int available(void);	// Check if new commands are available in buffer 
    IRdata read(void);		// Read command
    void flush(void);		// Flush FIFO receive buffer
  	  
    static void pinActivity();	// Static function only meant for pin change interrupt service routines (ISR)
  
  private:
    bool allowDuration(unsigned long duration, unsigned long expected);
    
    static IRreceiver* receivers[14];
    static int pinStates[14];
  	
    int pin;
    IRdata buffer[IRRECEIVER_BUFFERSIZE];
    unsigned int buffer_head;
    unsigned int buffer_tail;
  
    unsigned long prevTime;
    int state;
    char bitStream[32];
    int bitIndex;
  
    void updateState(int pinValue);
    void decodeNECBits(void);
};

IRreceiver* IRreceiver::receivers[14] = {NULL};
int IRreceiver::pinStates[14] = {0};

IRreceiver::IRreceiver(int pinNumber):	// Constructor
  pin(pinNumber), buffer_head(0), buffer_tail(0), state(IRRECEIVER_STATE_IDLE), bitIndex(0)
{
  pinMode(pin, INPUT);
  pinStates[pin] = digitalRead(pin);
  if (receivers[pin] != NULL) delete receivers[pin];	// if there was already another IRreceiver bind to this pin, delete that one
  receivers[pin] = this;
  
  *digitalPinToPCMSK(pin) |= bit(digitalPinToPCMSKbit(pin));  	// enable pin change interrupt mask bit for this pin
  PCIFR  |= bit(digitalPinToPCICRbit(pin)); 			// clear any outstanding interrupt
  PCICR  |= bit(digitalPinToPCICRbit(pin)); 			// enable interrupt for the group
}

IRreceiver::~IRreceiver()
{				// Destructor
  *digitalPinToPCMSK(pin) &= ~bit(digitalPinToPCMSKbit(pin));	// disable pin change interrupts
}

int IRreceiver::available(void)
{
  return (IRRECEIVER_BUFFERSIZE + buffer_head - buffer_tail) % IRRECEIVER_BUFFERSIZE;	
}

IRdata IRreceiver::read(void)
{
  IRdata result = {};
  if (buffer_head == buffer_tail)
  {	// if there is no data in ring buffer return an error object
    result.error = true;
  } 
  else
  {
    result = buffer[buffer_tail];
    buffer_tail = (buffer_tail + 1) % IRRECEIVER_BUFFERSIZE;
  }
  return result;
}

void IRreceiver::flush(void)
{
  buffer_tail = buffer_head;
}

void IRreceiver::updateState(int value)
{	// State machine to receive bits according to NEC protocol: http://www.sbprojects.com/knowledge/ir/nec.php
  unsigned long now = micros();
  unsigned long dur = now - prevTime; 		// duration of the previous burst or space 
  
  switch(state)
  {
  	case IRRECEIVER_STATE_IDLE:
      bitIndex = 0;
      if (value == 1 && dur > 5000) state = IRRECEIVER_STATE_START;	// start sequence at first burst after a gap (space wider than 5 ms)
      break;
    case IRRECEIVER_STATE_START:
      if (value == 0)
      {
        if (!allowDuration(dur, 9000)) state = IRRECEIVER_STATE_IDLE;	// a valid NEC sequence starts with a 9 ms AGC burst
      } else {
        if (allowDuration(dur, 4500)) state = IRRECEIVER_STATE_BITS;	// if the 9 ms AGC burst is followed by a 4.5 ms space: start receiving bits  
        else if (allowDuration(dur, 2250)) state = IRRECEIVER_STATE_REPEAT; // if it's followed by a 2.25 ms space: this is a repeat sequence
        else state = IRRECEIVER_STATE_IDLE;
      }    
      break;
    case IRRECEIVER_STATE_BITS:
      if (value == 1)
      {
        if (allowDuration(dur, 1690))
        {		// when a 560 us burst is followed by a 1690 us space, decode as logic 1 bit
          bitStream[bitIndex] = 1;
          bitIndex++;
        } else if (allowDuration(dur, 560))
        {// when a 560 us burst is followed by a 560 us space, decode as logic 0 bit
          bitStream[bitIndex] = 0;
          bitIndex++;
        } else state = IRRECEIVER_STATE_IDLE;	// error: return to idle        
      }
    else 
    {
        if (allowDuration(dur, 560))
        {	// check if bit start burst is 560 us long
          if(bitIndex == 32)
          {			// Sequence ends after receiving 32 bits
          	this->decodeNECBits();		// decode received bitstream
          	state = IRRECEIVER_STATE_IDLE;
          }
        }
      else state = IRRECEIVER_STATE_IDLE;
      }
	  break;
    case IRRECEIVER_STATE_REPEAT:
      if (allowDuration(dur, 560)) this->decodeNECBits();
      state = IRRECEIVER_STATE_IDLE;
      break;
  }
  prevTime = now;
}
                   
bool IRreceiver::allowDuration(unsigned long duration, unsigned long expected)
{    
	return (duration > expected*3/4 && duration < expected*5/4);	// allow 25 % deviation on burst and space durations 
}
                   
void IRreceiver::decodeNECBits()
{
  IRdata result = {};
  if (bitIndex == 0) result.repeat = true;	// when no bits are received this is a repeat code
  else {
    for (int i=0; i<bitIndex; i++)
    {			// convert bitstream to bytes (LSB first)
      result.rawBytes[i>>3] |= bitStream[i] << (i&7);
    }
    result.address = result.rawBytes[0] | result.rawBytes[1] << 8;	// Extended NEC protocol: first 2 bytes represents the address
    result.command = result.rawBytes[2];	// 3th byte represents the command 
    unsigned char commandInverse = ~result.rawBytes[3];				// 4th byte is again the command byte but all bits are inverted
    if (result.rawBytes[2] != commandInverse) result.error = true;	// verify the command
  }
  
  int newHead = (buffer_head + 1) % IRRECEIVER_BUFFERSIZE;	// if buffer is not full: add data to buffer, otherwise discard data
  if (newHead != buffer_tail){
    buffer[buffer_head] = result;
  	buffer_head = newHead;
  }  
}
                   
void IRreceiver::pinActivity(){
  for (int i=0; i<14; i++){
    int value = digitalRead(i);	
    if (value != pinStates[i] && receivers[i] != NULL && (*digitalPinToPCMSK(i) & bit(digitalPinToPCMSKbit(i)))){ // check which pin has toggled 
    	receivers[i]->updateState(!value);		// if a receiver is defined for this pin: update its state with the current value (Note: IR receiver output is active low)
    }
    pinStates[i]=value;
  }
}

ISR(PCINT0_vect){IRreceiver::pinActivity();}	// Add ISR to all pin change interrupts
ISR(PCINT1_vect){IRreceiver::pinActivity();}
ISR(PCINT2_vect){IRreceiver::pinActivity();}

// End of IRremote library
//------------------------

/*---------------------------------------------------------------------------------------------------------------------*/


#include <LiquidCrystal.h>
#define PIN_LED 10
#define PIN_IR  9
#define VELOCIDAD 500 // Velocidad para mover el texto
const int pin13=13;
const int pin8=8;
const int pin7=7;
const int pin6=6;
const int pin10=10;


LiquidCrystal lcd(12, 11, 5, 4, 3, 2); // initialize the library with the numbers of the interface pins
IRreceiver receiver(PIN_IR);   // Create an IR receiver

char *questions[5]= {"Question 1","Question 2","Question 3","Question 4","Question 5"};
int  *questionPosition = 0;

void setup()
{
  // set up the LCD's number of columns and rows:
  lcd.begin(16, 2);
  pinMode(pin13,OUTPUT);
  pinMode(pin8,OUTPUT);
  pinMode(pin7,OUTPUT);
  pinMode(pin6,OUTPUT);
  pinMode(PIN_LED, OUTPUT); 
  Serial.begin(9600); 
  				
}

void loop()
{
  
  if (receiver.available()) 
  {       // If data is available
    
    IRdata data = receiver.read();  // Read the received command from the IR buffer
    
    if (!data.repeat)
    {             // Only act on a new button push to toggle the LED.
      
      lcd.clear();  
      
      switch(data.command)
      {
        case 0: (*questionPosition)++; nextQuestion(); break;
        
       case 26: if (!data.repeat)
       {
        lcd.print("button-9=all off");
        digitalWrite(pin13,LOW);
        digitalWrite(pin7,LOW);
        digitalWrite(pin6,LOW);
        digitalWrite(pin10,LOW);
        
         
        lcd.clear();
        lcd.blink();     
        lcd.setCursor(0,1);
        lcd.print("ENTER-1=ON TV"); 
        delay(1000);
        lcd.clear();
        lcd.blink();
        lcd.setCursor(0,1);
        lcd.print("ENTER-2=OFF TV"); 
        delay(1000);
        lcd.clear();
        lcd.blink();    
        lcd.print("ENTER 3=ON LIGHT"); 
        delay(1000);
        lcd.clear();
        lcd.blink();
        lcd.setCursor(0,1);
        lcd.print("ENTER 4=OFF LIGHT"); 
        delay(1000);
        lcd.clear();
        lcd.blink();       
        lcd.print("ENTER 5=ON FAN");
        delay(1000);
        lcd.clear();
        lcd.blink();
        lcd.setCursor(0,1);
        lcd.print("ENTER 6=OFF FAN"); 
        delay(1000);
        lcd.clear();
        lcd.blink();   
        lcd.print("ENTER 8=ON MOTOR");
        delay(1000);
        lcd.clear();
        lcd.blink();
        lcd.setCursor(0,1);
        lcd.print("ENTER 7=OFF MOTOR"); 
        
       }
        else
        {
          digitalWrite(pin13,HIGH);
        digitalWrite(pin7,HIGH);
        digitalWrite(pin6,HIGH);
        digitalWrite(pin10,HIGH);
       
        }
        
        break;
   
      	case 16: 
        lcd.print("BUTTON-1=TV ON");
        digitalWrite(pin13,HIGH);
       	delay(1000);
        lcd.clear();
        lcd.blink();
        lcd.print("TV IS ON");
        lcd.setCursor(0,1);
        lcd.print("ENTER-2=OFF TV");
        delay(1000);
        break;
        case 17: lcd.print("TV IS OFF ");
        digitalWrite(pin13,LOW); break;
        
        
        case 18: lcd.print("LIGHT ON");     
        digitalWrite(pin7,HIGH);     
        delay(100);
        lcd.clear();
        lcd.blink();
        lcd.print("LIGHT ON,ENTER"); 
        lcd.setCursor(0,1);
        lcd.print("-4 TO OFF LIGHT"); 
        delay(100);
        break;            
       case 20: lcd.print("LIGHT OFF");
        digitalWrite(pin7,LOW);
        delay(100);
        lcd.clear();
        lcd.blink();
        lcd.print("LIGHT OFF,ENTER"); 
        lcd.setCursor(0,1);
        lcd.print("-3 TO ON LIGHT"); 
        delay(1000);
        lcd.clear();
        lcd.blink();     
        lcd.setCursor(0,1);
        lcd.print("ENTER-1=ON TV"); 
        delay(1000);
        lcd.clear();
        lcd.blink();
        lcd.setCursor(0,1);
        lcd.print("ENTER-2=OFF TV"); 
        delay(1000);
        lcd.clear();
        lcd.blink();    
        lcd.print("ENTER 3=ON LIGHT"); 
        delay(1000);
        lcd.clear();
        lcd.blink();
        lcd.setCursor(0,1);
        lcd.print("ENTER 4=OFF LIGHT"); 
        delay(1000);
        lcd.clear();
        lcd.blink();       
        lcd.print("ENTER 5=ON FAN");
        delay(1000);
        lcd.clear();
        lcd.blink();
        lcd.setCursor(0,1);
        lcd.print("ENTER 6=OFF FAN"); 
        delay(1000);
        lcd.clear();
        lcd.blink();   
        lcd.print("ENTER 8=ON MOTOR");
        delay(1000);
        lcd.clear();
        lcd.blink();
        lcd.setCursor(0,1);
        lcd.print("ENTER 7=OFF MOTOR"); 
     
      
 
       break;
    
        
        
        case 21: lcd.print("PIN-5=FAN is ON");
        digitalWrite(pin6,HIGH);
        delay(100);
        lcd.clear();
        lcd.blink();
        lcd.print("LIGHT OFF,ENTER"); 
        lcd.setCursor(0,1);
        lcd.print("-3 TO ON LIGHT"); 
        delay(1000);
        lcd.clear();
        lcd.blink();     
        lcd.setCursor(0,1);
        lcd.print("ENTER-1=ON TV"); 
        delay(1000);
        lcd.clear();
        lcd.blink();
        lcd.setCursor(0,1);
        lcd.print("ENTER-2=OFF TV"); 
        delay(1000);
        lcd.clear();
        lcd.blink();    
        lcd.print("ENTER 3=ON LIGHT"); 
        delay(1000);
        lcd.clear();
        lcd.blink();
        lcd.setCursor(0,1);
        lcd.print("ENTER 4=OFF LIGHT"); 
        delay(1000);
        lcd.clear();
        lcd.blink();       
        lcd.print("ENTER 5=ON FAN");
        delay(1000);
        lcd.clear();
        lcd.blink();
        lcd.setCursor(0,1);
        lcd.print("ENTER 6=OFF FAN"); 
        delay(1000);
        lcd.clear();
        lcd.blink();   
        lcd.print("ENTER 8=ON MOTOR");
        delay(1000);
        lcd.clear();
        lcd.blink();
        lcd.setCursor(0,1);
        lcd.print("ENTER 7=OFF MOTOR");
        
        break;
        case 22: lcd.print("PIN-6=FAN is OFF");
        digitalWrite(pin6,LOW);
        delay(100);
        lcd.clear();
        lcd.blink();
        lcd.print("LIGHT OFF,ENTER"); 
        lcd.setCursor(0,1);
        lcd.print("-3 TO ON LIGHT"); 
        delay(1000);
        lcd.clear();
        lcd.blink();     
        lcd.setCursor(0,1);
        lcd.print("ENTER-1=ON TV"); 
        delay(1000);
        lcd.clear();
        lcd.blink();
        lcd.setCursor(0,1);
        lcd.print("ENTER-2=OFF TV"); 
        delay(1000);
        lcd.clear();
        lcd.blink();    
        lcd.print("ENTER 3=ON LIGHT"); 
        delay(1000);
        lcd.clear();
        lcd.blink();
        lcd.setCursor(0,1);
        lcd.print("ENTER 4=OFF LIGHT"); 
        delay(1000);
        lcd.clear();
        lcd.blink();       
        lcd.print("ENTER 5=ON FAN");
        delay(1000);
        lcd.clear();
        lcd.blink();
        lcd.setCursor(0,1);
        lcd.print("ENTER 6=OFF FAN"); 
        delay(1000);
        lcd.clear();
        lcd.blink();   
        lcd.print("ENTER 8=ON MOTOR");
        delay(1000);
        lcd.clear();
        lcd.blink();
        lcd.setCursor(0,1);
        lcd.print("ENTER 7=OFF MOTOR");
        
        break;        
        case 25: lcd.print("PIN-10=MOTOR ON " );
        digitalWrite(pin10,HIGH);
        delay(100);
        lcd.clear();
        lcd.blink();
        lcd.print("LIGHT OFF,ENTER"); 
        lcd.setCursor(0,1);
        lcd.print("-3 TO ON LIGHT"); 
        delay(1000);
        lcd.clear();
        lcd.blink();     
        lcd.setCursor(0,1);
        lcd.print("ENTER-1=ON TV"); 
        delay(1000);
        lcd.clear();
        lcd.blink();
        lcd.setCursor(0,1);
        lcd.print("ENTER-2=OFF TV"); 
        delay(1000);
        lcd.clear();
        lcd.blink();    
        lcd.print("ENTER 3=ON LIGHT"); 
        delay(1000);
        lcd.clear();
        lcd.blink();
        lcd.setCursor(0,1);
        lcd.print("ENTER 4=OFF LIGHT"); 
        delay(1000);
        lcd.clear();
        lcd.blink();       
        lcd.print("ENTER 5=ON FAN");
        delay(1000);
        lcd.clear();
        lcd.blink();
        lcd.setCursor(0,1);
        lcd.print("ENTER 6=OFF FAN"); 
        delay(1000);
        lcd.clear();
        lcd.blink();   
        lcd.print("ENTER 8=ON MOTOR");
        delay(1000);
        lcd.clear();
        lcd.blink();
        lcd.setCursor(0,1);
        lcd.print("ENTER 7=OFF MOTOR");
        break;
        case 24:lcd.print("PIN-10=MOTOR OFF");
        digitalWrite(pin10,LOW);
        delay(100);
        lcd.clear();
        lcd.blink();
        lcd.print("LIGHT OFF,ENTER"); 
        lcd.setCursor(0,1);
        lcd.print("-3 TO ON LIGHT"); 
        delay(1000);
        lcd.clear();
        lcd.blink();     
        lcd.setCursor(0,1);
        lcd.print("ENTER-1=ON TV"); 
        delay(1000);
        lcd.clear();
        lcd.blink();
        lcd.setCursor(0,1);
        lcd.print("ENTER-2=OFF TV"); 
        delay(1000);
        lcd.clear();
        lcd.blink();    
        lcd.print("ENTER 3=ON LIGHT"); 
        delay(1000);
        lcd.clear();
        lcd.blink();
        lcd.setCursor(0,1);
        lcd.print("ENTER 4=OFF LIGHT"); 
        delay(1000);
        lcd.clear();
        lcd.blink();       
        lcd.print("ENTER 5=ON FAN");
        delay(1000);
        lcd.clear();
        lcd.blink();
        lcd.setCursor(0,1);
        lcd.print("ENTER 6=OFF FAN"); 
        delay(1000);
        lcd.clear();
        lcd.blink();   
        lcd.print("ENTER 8=ON MOTOR");
        delay(1000);
        lcd.clear();
        lcd.blink();
        lcd.setCursor(0,1);
        lcd.print("ENTER 7=OFF MOTOR");
        
        break;
        
        
		default: lcd.print("Not found"); 
        delay(100);
        lcd.clear();
        lcd.blink();
        lcd.print("LIGHT OFF,ENTER"); 
        lcd.setCursor(0,1);
        lcd.print("-3 TO ON LIGHT"); 
        delay(1000);
        lcd.clear();
        lcd.blink();     
        lcd.setCursor(0,1);
        lcd.print("ENTER-1=ON TV"); 
        delay(1000);
        lcd.clear();
        lcd.blink();
        lcd.setCursor(0,1);
        lcd.print("ENTER-2=OFF TV"); 
        delay(1000);
        lcd.clear();
        lcd.blink();    
        lcd.print("ENTER 3=ON LIGHT"); 
        delay(1000);
        lcd.clear();
        lcd.blink();
        lcd.setCursor(0,1);
        lcd.print("ENTER 4=OFF LIGHT"); 
        delay(1000);
        lcd.clear();
        lcd.blink();       
        lcd.print("ENTER 5=ON FAN");
        delay(1000);
        lcd.clear();
        lcd.blink();
        lcd.setCursor(0,1);
        lcd.print("ENTER 6=OFF FAN"); 
        delay(1000);
        lcd.clear();
        lcd.blink();   
        lcd.print("ENTER 8=ON MOTOR");
        delay(1000);
        lcd.clear();
        lcd.blink();
        lcd.setCursor(0,1);
        lcd.print("ENTER 7=OFF MOTOR");
        
        break;
      }  
    }
  }
  delay(100);  // wait for a while
  
}

void nextQuestion(){
  lcd.clear();
  int p = *questionPosition;
  lcd.print(questions[p]);
  delay(100);
}
