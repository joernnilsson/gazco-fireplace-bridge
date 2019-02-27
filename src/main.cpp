#include <Homie.h>



// TODO Overflow protection on micros()
// TODO Support secure mqtt
// TODO Fix timeout hack in IotWebConf lib
// TODO Present a better index.html, with fireplace info/controls
// TODO Support reset button
// TODO Check for mqtt connection befor sending

// ***** Fireplace Config

#define TX                D2
#define CLK_PERIOD_US     1023


#define LEVEL_0_AUTO      0x381e34c41
#define LEVEL_3_AUTO      0x143234c41
#define LEVEL_0_STD       0x340224c41
#define LEVEL_1_STD       0x40124c41
#define LEVEL_2_STD       0x240324c41
#define LEVEL_3_STD       0x1c00a4c41

#define CMD_REPEAT_N      5
#define CMD_REPEAT_DELAY  23

#define CTRL_MODE_OFF     0
#define CTRL_MODE_MANUAL  1
#define CTRL_MODE_AUTO    2

#define FP_STATE_UNKNOWN  0
#define FP_STATE_0        1
#define FP_STATE_1        2
#define FP_STATE_2        3
#define FP_STATE_3        4

int8_t fp_state = FP_STATE_UNKNOWN;
int8_t ctrl_mode = CTRL_MODE_MANUAL;

uint32_t us_start;
uint32_t us_up;

int8_t last_fp_state = -1;
int8_t last_ctrl_mode = -1;
bool schedule_sync = false;


HomieNode node("fireplace", "Fireplace", "fireplace");

int8_t scheduled_command = -1;


/**
 * Transmit the command.
 * Basically a bitbanging spi line.
 */
void set_level(uint64_t seq){

  us_start = micros();

  for(int i=0; i < 34; i++){

    uint8_t state = ((seq >> i) & 0x01);
    digitalWrite(TX, state);
    
    // Wait for next transition
    us_up = us_start + (i+1)*CLK_PERIOD_US;
    while(micros() < us_up){
      // nop
    }
  }
  
  digitalWrite(TX, LOW);

  schedule_sync = true;
}

void set_level_n(uint64_t seq){
  
  
  for(int i=0; i < CMD_REPEAT_N; i++){
    set_level(seq);
    delay(CMD_REPEAT_DELAY);
  }

}

void executeCmd(int cmd){
  Serial.print("Executing command: ");
  Serial.println(cmd, HEX);
  switch(cmd){
      case 0:
        set_level_n(LEVEL_0_STD);
        fp_state = FP_STATE_0;
        break;
      case 1:
        set_level_n(LEVEL_1_STD);
        fp_state = FP_STATE_1;
        break;
      case 2:
        set_level_n(LEVEL_2_STD);
        fp_state = FP_STATE_2;
        break;
      case 3:
        set_level_n(LEVEL_3_STD);
        fp_state = FP_STATE_3;
        break;
      case 4:
        set_level_n(LEVEL_0_AUTO);
        fp_state = FP_STATE_0;
        break;
      case 5:
        set_level_n(LEVEL_3_AUTO);
        fp_state = FP_STATE_3;
        break;
      default:
        Serial.print("Unknown command: ");
        Serial.println(cmd);
    }
}

void pt(char * str, uint32_t t){
  Serial.print(str);
  Serial.print(": ");
  Serial.println(t);
}

uint32_t last_sync;
void loopHandler() {


  if(Serial.available()){
    String str_input = Serial.readStringUntil('\n');
    Serial.println(str_input);
    pt("Line len", str_input.length());
    int input = str_input.toInt();
    pt("Got line", input);
  
    executeCmd(input);
    schedule_sync = true;
  }

  if(scheduled_command >= 0){
    executeCmd(scheduled_command);
    scheduled_command = -1;
    schedule_sync = true;
  }

  if(millis() - last_sync > 2000 || schedule_sync)  {

    if(true){
      int level = 0;
      switch(fp_state){
        case FP_STATE_0:
          level = 0;
          break;
        case FP_STATE_1:
          level = 33;
          break;
        case FP_STATE_2:
          level = 67;
          break;
        case FP_STATE_3:
          level = 100;
          break;
      }
      
      node.setProperty("level").send(String(fp_state-1));
      node.setProperty("level-percent").send(String(level));

      last_fp_state = fp_state;
    }
    
    if(true){   
      node.setProperty("mode").send(String(ctrl_mode));
    }

    schedule_sync = false;
    last_sync = millis();
  }



}

bool levelHandler(const HomieRange& range, const String& value) {

  Serial.print("Incoming: ");
  Serial.println(value);
  int cmd = value.toInt();

  scheduled_command = cmd;

  return true;
}



void setup() {

  Serial.begin(115200);
  Serial.println();
  Serial.println("Starting up...");


  // ***** Fireplace setup

  pinMode(TX, OUTPUT);
  digitalWrite(TX, LOW);


  // ***** Mqtt setup

  Homie_setFirmware("fireplace", "2.0.0");
  Homie.setLoopFunction(loopHandler);

  node.advertise("level").setName("Level")
                                .setDatatype("integer")
                                .setFormat("0:3")
                                .setRetained(true)
                                .settable(levelHandler);

  node.advertise("level-percent").setName("Level percent")
                                .setDatatype("integer")
                                .setFormat("0:100")
                                .setRetained(true);

  node.advertise("mode").setName("Control mode")
                                .setDatatype("integer")
                                .setFormat("0:3")
                                .setRetained(true);

  Homie.setup();

  Serial.println("Ready");
}

void loop() {
  Homie.loop();
}