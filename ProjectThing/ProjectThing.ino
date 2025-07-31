// ProjectThing.ino ////////////////////////////////////////////////////////////////
// Budget Expenses Tracker //////////////////////////////////////////////////////
//
// The user can input their expenses and track their budget on the unPhone 9
// automatically synced with google sheets
//
// we use the touchscreen (XPT2046) driver from the unPhone library (with the
// TFT driver from TFT_eSPI)

#include "unPhone.h"
#include <Adafruit_SPIFlash.h> // for LDF

#if __has_include("private.h")
#  include "private.h"
#else
#  error "private.h is missing, copy private-template.h and fill it in"
#endif

#include <lvgl.h>                       // LVGL //////////////////////////////
#define CONFIG_IDF_TARGET_ESP32S3 1
#include <TFT_eSPI.h>

#include <ESP_Google_Sheet_Client.h>

#include <WiFi.h>
#include <time.h>

// create an unPhone; add a custom version of Arduino's map command for
// translating from touchscreen coordinates to LCD coordinates
unPhone u = unPhone();
long my_mapper(long, long, long, long, long);

/*Change to your screen resolution*/
static const uint16_t screenWidth  = 480;
static const uint16_t screenHeight = 320;

static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf[ screenWidth * 10 ];

TFT_eSPI tft = TFT_eSPI(screenWidth, screenHeight); // the LCD screen

lv_obj_t *screens[5];
int current_screen = 0;

lv_indev_t *keypad_indev = NULL;

#if LV_USE_LOG != 0
/* Serial debugging */
void my_print(const char * buf) {
  Serial.printf(buf);
  Serial.flush();
}
#endif

/* Display flushing */
void my_disp_flush(
  lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p
) {
  uint32_t w = ( area->x2 - area->x1 + 1 );
  uint32_t h = ( area->y2 - area->y1 + 1 );

  tft.startWrite();
  tft.setAddrWindow( area->x1, area->y1, w, h );
  tft.pushColors( ( uint16_t * )&color_p->full, w * h, true );
  tft.endWrite();

  lv_disp_flush_ready( disp );
}

// map touch coords to lcd coords
// a version of map that never returns out of range values
long my_mapper(long x, long in_min, long in_max, long out_min, long out_max) {
  long probable =
  (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
  if(probable < out_min) return out_min;
  if(probable > out_max) return out_max;
  return probable;
}

/*Read the touchpad*/
void my_touchpad_read(lv_indev_drv_t * indev_driver, lv_indev_data_t * data) {
  uint16_t touchX, touchY;

  // start of changes for unPhone ////////////////////////////////////////////
  bool touched = u.tsp->touched();

  if( !touched ) {
    data->state = LV_INDEV_STATE_REL;
  } else {
    data->state = LV_INDEV_STATE_PR;

    /*Set the coordinates*/
    TS_Point p(-1, -1, -1);
    p = u.tsp->getPoint();

// filter the ghosting on version 9 boards (on USB power; ~300 pressure)
#if UNPHONE_SPIN >= 9
    if(p.z < 400) return;
//  D("probable ghost reject @ p.x(%04d), p.y(%04d) p.z(%04d)\n", p.x,p.y,p.z)
#endif

    Serial.printf("   p.x(%04d),  p.y(%04d) p.z(%04d)\n", p.x, p.y, p.z);
    if(p.x < 0 || p.y < 0) D("************* less than zero! *************\n")

    long xMin = 320;
    long xMax = 3945;
    long yMin = 420;
    long yMax = 3915;

    long xscld = my_mapper((long) p.x, xMin, xMax, 0, (long) screenWidth);
    long yscld = // Y is inverted on rotation 1 (landscape, buttons right)
      ((long) screenHeight) - 
      my_mapper((long) p.y, yMin, yMax, 0, (long) screenHeight);
    touchX = (uint16_t) xscld;
    touchY = (uint16_t) yscld;

    Serial.printf("touchX(%4d), touchY(%4d)\n", touchX, touchY);
    // end of changes for unPhone ////////////////////////////////////////////

    data->point.x = touchX;
    data->point.y = touchY;
    Serial.printf("Data x %u, Data y %u\n", touchX, touchY);
  }
}

// Global vars to track totals and UI elements
float budget_groceries;
float budget_leisure;
float budget_travel;
float budget_other;

float total_spent_groceries;
float total_spent_leisure;
float total_spent_travel;
float total_spent_other;

float remaining_groceries;
float remaining_leisure;
float remaining_travel;
float remaining_other;

lv_obj_t *ta_groceries, *ta_leisure, *ta_travel, *ta_other;
lv_obj_t *label_groceries_total, *label_leisure_total, *label_travel_total, *label_other_total;
lv_obj_t *archive_groceries, *archive_leisure, *archive_travel, *archive_other;

lv_obj_t *label_groceries_remaining;
lv_obj_t *label_leisure_remaining;
lv_obj_t *label_travel_remaining;
lv_obj_t *label_other_remaining;


lv_obj_t *keypad;

// Handles creation of the main GUI which includes the 5 screens
void createGUI() {
  screens[0] = lv_obj_create(NULL); // Groceries
  screens[1] = lv_obj_create(NULL); // Leisure
  screens[2] = lv_obj_create(NULL); // Travel
  screens[3] = lv_obj_create(NULL); // Other
  screens[4] = lv_obj_create(NULL); // Archive

  // ========== GROCERIES SCREEN ==========
  lv_obj_t *label_groceries = lv_label_create(screens[0]);
  lv_label_set_text(label_groceries, "Groceries");
  lv_obj_align(label_groceries, LV_ALIGN_TOP_LEFT, 10, 20);

  lv_obj_t *input_label_groceries = lv_label_create(screens[0]);
  lv_label_set_text(input_label_groceries, "Input:");
  lv_obj_align(input_label_groceries, LV_ALIGN_TOP_RIGHT, -120, 35);
  
  ta_groceries = lv_textarea_create(screens[0]);
  lv_obj_set_width(ta_groceries, 150);
  lv_obj_align(ta_groceries, LV_ALIGN_TOP_RIGHT, -20, 75);
  lv_textarea_set_placeholder_text(ta_groceries, "0");
  lv_textarea_set_one_line(ta_groceries, true);
  lv_obj_add_event_cb(ta_groceries, ta_event_cb, LV_EVENT_FOCUSED, NULL);

  lv_obj_t *label_groceries_budget = lv_label_create(screens[0]);
  lv_label_set_text_fmt(label_groceries_budget, "Budget: %s", format_money(budget_groceries));
  lv_obj_align(label_groceries_budget, LV_ALIGN_TOP_LEFT, 10, 50);

  label_groceries_total = lv_label_create(screens[0]);
  lv_label_set_text_fmt(label_groceries_total, "Total Spent: %s", format_money(total_spent_groceries));
  lv_obj_align(label_groceries_total, LV_ALIGN_TOP_LEFT, 10, 80);

  label_groceries_remaining = lv_label_create(screens[0]);
  lv_label_set_text_fmt(label_groceries_remaining, "Remaining: %s", format_money(remaining_groceries));
  lv_obj_align(label_groceries_remaining, LV_ALIGN_TOP_LEFT, 10, 110);

  // ========== LEISURE SCREEN ==========
  lv_obj_t *label_leisure = lv_label_create(screens[1]);
  lv_label_set_text(label_leisure, "Leisure");
  lv_obj_align(label_leisure, LV_ALIGN_TOP_LEFT, 10, 20);

  lv_obj_t *input_label_leisure = lv_label_create(screens[1]);
  lv_label_set_text(input_label_leisure, "Input:");
  lv_obj_align(input_label_leisure, LV_ALIGN_TOP_RIGHT, -120, 35);

  ta_leisure = lv_textarea_create(screens[1]);
  lv_obj_set_width(ta_leisure, 150);
  lv_obj_align(ta_leisure, LV_ALIGN_TOP_RIGHT, -20, 75);
  lv_textarea_set_placeholder_text(ta_leisure, "0");
  lv_textarea_set_one_line(ta_leisure, true);
  lv_obj_add_event_cb(ta_leisure, ta_event_cb, LV_EVENT_FOCUSED, NULL);

  lv_obj_t *label_leisure_budget = lv_label_create(screens[1]);
  lv_label_set_text_fmt(label_leisure_budget, "Budget: %s", format_money(budget_leisure));
  lv_obj_align(label_leisure_budget, LV_ALIGN_TOP_LEFT, 10, 50);

  label_leisure_total = lv_label_create(screens[1]);
  lv_label_set_text_fmt(label_leisure_total, "Total Spent: %s", format_money(total_spent_leisure));
  lv_obj_align(label_leisure_total, LV_ALIGN_TOP_LEFT, 10, 80);

  label_leisure_remaining = lv_label_create(screens[1]);
  lv_label_set_text_fmt(label_leisure_remaining, "Remaining: %s", format_money(remaining_leisure));
  lv_obj_align(label_leisure_remaining, LV_ALIGN_TOP_LEFT, 10, 110);

  // ========== TRAVEL SCREEN ==========
  lv_obj_t *label_travel = lv_label_create(screens[2]);
  lv_label_set_text(label_travel, "Travel");
  lv_obj_align(label_travel, LV_ALIGN_TOP_LEFT, 10, 20);

  lv_obj_t *input_label_travel = lv_label_create(screens[2]);
  lv_label_set_text(input_label_travel, "Input:");
  lv_obj_align(input_label_travel, LV_ALIGN_TOP_RIGHT, -120, 35);

  ta_travel = lv_textarea_create(screens[2]);
  lv_obj_set_width(ta_travel, 150);
  lv_obj_align(ta_travel, LV_ALIGN_TOP_RIGHT, -20, 75);
  lv_textarea_set_placeholder_text(ta_travel, "0");
  lv_textarea_set_one_line(ta_travel, true);
  lv_obj_add_event_cb(ta_travel, ta_event_cb, LV_EVENT_FOCUSED, NULL);

  lv_obj_t *label_travel_budget = lv_label_create(screens[2]);
  lv_label_set_text_fmt(label_travel_budget, "Budget: %s", format_money(budget_travel));
  lv_obj_align(label_travel_budget, LV_ALIGN_TOP_LEFT, 10, 50);

  label_travel_total = lv_label_create(screens[2]);
  lv_label_set_text_fmt(label_travel_total, "Total Spent: %s", format_money(total_spent_travel));
  lv_obj_align(label_travel_total, LV_ALIGN_TOP_LEFT, 10, 80);

  label_travel_remaining = lv_label_create(screens[2]);
  lv_label_set_text_fmt(label_travel_remaining, "Remaining: %s", format_money(remaining_travel));
  lv_obj_align(label_travel_remaining, LV_ALIGN_TOP_LEFT, 10, 110);

  // ========== OTHER SCREEN ==========
  lv_obj_t *label_other = lv_label_create(screens[3]);
  lv_label_set_text(label_other, "Other");
  lv_obj_align(label_other, LV_ALIGN_TOP_LEFT, 10, 20);

  lv_obj_t *input_label_other = lv_label_create(screens[3]);
  lv_label_set_text(input_label_other, "Input:");
  lv_obj_align(input_label_other, LV_ALIGN_TOP_RIGHT, -120, 35);

  ta_other = lv_textarea_create(screens[3]);
  lv_obj_set_width(ta_other, 150);
  lv_obj_align(ta_other, LV_ALIGN_TOP_RIGHT, -20, 75);
  lv_textarea_set_placeholder_text(ta_other, "0");
  lv_textarea_set_one_line(ta_other, true);
  lv_obj_add_event_cb(ta_other, ta_event_cb, LV_EVENT_FOCUSED, NULL);

  lv_obj_t *label_other_budget = lv_label_create(screens[3]);
  lv_label_set_text_fmt(label_other_budget, "Budget: %s", format_money(budget_other));
  lv_obj_align(label_other_budget, LV_ALIGN_TOP_LEFT, 10, 50);

  label_other_total = lv_label_create(screens[3]);
  lv_label_set_text_fmt(label_other_total, "Total Spent: %s", format_money(total_spent_other));
  lv_obj_align(label_other_total, LV_ALIGN_TOP_LEFT, 10, 80);

  label_other_remaining = lv_label_create(screens[3]);
  lv_label_set_text_fmt(label_other_remaining, "Remaining: %s", format_money(remaining_other));
  lv_obj_align(label_other_remaining, LV_ALIGN_TOP_LEFT, 10, 110);

  // ========== ARCHIVE SCREEN ==========
  lv_obj_t *label_archive = lv_label_create(screens[4]);
  lv_label_set_text(label_archive, "Archive");
  lv_obj_align(label_archive, LV_ALIGN_TOP_LEFT, 10, 20);

  lv_obj_t *btn_archive = lv_btn_create(screens[4]);
  lv_obj_set_size(btn_archive, 180, 40);
  lv_obj_align(btn_archive, LV_ALIGN_RIGHT_MID, -20, -40);
  lv_obj_add_event_cb(btn_archive, archive_and_reset_cb, LV_EVENT_CLICKED, NULL);  

  lv_obj_t *label_btn = lv_label_create(btn_archive);
  lv_label_set_text(label_btn, "Archive and reset totals");
  lv_obj_center(label_btn);

  // groceries
  archive_groceries = lv_label_create(screens[4]);
  lv_label_set_text_fmt(archive_groceries, "Total Spent Groceries: %s", format_money(total_spent_groceries));
  lv_obj_align(archive_groceries, LV_ALIGN_TOP_LEFT, 10, 50); 

  // leisure
  archive_leisure = lv_label_create(screens[4]);
  lv_label_set_text_fmt(archive_leisure, "Total Spent Leisure: %s", format_money(total_spent_leisure));
  lv_obj_align(archive_leisure, LV_ALIGN_TOP_LEFT, 10, 80);

  // travel
  archive_travel = lv_label_create(screens[4]);
  lv_label_set_text_fmt(archive_travel, "Total Spent Travel: %s", format_money(total_spent_travel));
  lv_obj_align(archive_travel, LV_ALIGN_TOP_LEFT, 10, 110); 

  // other
  archive_other = lv_label_create(screens[4]);
  lv_label_set_text_fmt(archive_other, "Total Spent Other: %s", format_money(total_spent_other));
  lv_obj_align(archive_other, LV_ALIGN_TOP_LEFT, 10, 140);  

  lv_scr_load(screens[0]);  // Show groceries by default
}

void archive_and_reset_cb(lv_event_t *e) {
  Serial.println("Archive and reset button clicked");

  if (GSheet.ready()) {
    // append to archive
    String now = get_current_date();
    append_totals_to_sheet2(total_spent_groceries, total_spent_leisure, total_spent_travel, total_spent_other, now);
    
    // reset
    reset_category(&total_spent_groceries, "Sheet1!B3", "Sheet1!B2", label_groceries_total, label_groceries_remaining);
    reset_category(&total_spent_leisure,  "Sheet1!C3", "Sheet1!C2", label_leisure_total, label_leisure_remaining);
    reset_category(&total_spent_travel,   "Sheet1!D3", "Sheet1!D2", label_travel_total, label_travel_remaining);
    reset_category(&total_spent_other,    "Sheet1!E3", "Sheet1!E2", label_other_total, label_other_remaining);

    update_archive_totals();
  } else {
      Serial.println("GSheet not ready. Could not archive values.");
  }
}

void ta_event_cb(lv_event_t *e) {
  lv_obj_t *ta = lv_event_get_target(e);
    
  if (!keypad) {
    keypad = lv_keyboard_create(lv_scr_act());
    lv_keyboard_set_mode(keypad, LV_KEYBOARD_MODE_NUMBER);
        
    static const char * keymap[] = {
      "1", "2", "3", "\n",
      "4", "5", "6", "\n", 
      "7", "8", "9", "\n",
      LV_SYMBOL_BACKSPACE, "0", ".", ""
    };
        
    static const lv_btnmatrix_ctrl_t ctrl_map[] = {
      1, 1, 1,
      1, 1, 1,
      1, 1, 1,
      2, 1, 1
    };
        
    lv_keyboard_set_map(keypad, LV_KEYBOARD_MODE_NUMBER, keymap, ctrl_map);
    lv_keyboard_set_textarea(keypad, ta);
    lv_obj_align(keypad, LV_ALIGN_BOTTOM_MID, 0, 0);
  } else {
    lv_keyboard_set_textarea(keypad, ta);
    lv_obj_set_parent(keypad, lv_scr_act());
  }
}

// Handles inputted expense
void add_amount_from_button() {
  lv_obj_t *ta = NULL;
  float *total = NULL;
  lv_obj_t *label_total = NULL;
  lv_obj_t *label_remaining = NULL;
  String cellRangeUpdate;
  String cellRangeRemaining;

  switch(current_screen) {
    case 0: // Groceries
      ta = ta_groceries;
      total = &total_spent_groceries;
      label_total = label_groceries_total;
      label_remaining = label_groceries_remaining;
      cellRangeRemaining = "Sheet1!B4";
      cellRangeUpdate = "Sheet1!B3";
      break;
    case 1: // Leisure
      ta = ta_leisure;
      total = &total_spent_leisure;
      label_total = label_leisure_total;
      label_remaining = label_leisure_remaining;
      cellRangeRemaining = "Sheet1!C4";
      cellRangeUpdate = "Sheet1!C3";
      break;
    case 2: // Travel
      ta = ta_travel;
      total = &total_spent_travel;
      label_total = label_travel_total;
      label_remaining = label_travel_remaining;
      cellRangeRemaining = "Sheet1!D4";
      cellRangeUpdate = "Sheet1!D3";
      break;
    case 3: // Other
      ta = ta_other;
      total = &total_spent_other;
      label_total = label_other_total;
      label_remaining = label_other_remaining;
      cellRangeRemaining = "Sheet1!E4";
      cellRangeUpdate = "Sheet1!E3";
      break;
  }

  if(ta == NULL || total == NULL) return;

  const char* text = lv_textarea_get_text(ta);
  float value = atof(text);

  if(value > 0) {
    *total += value;

    if (GSheet.ready()) {
      update_cell_with_float(*total, cellRangeUpdate);
      float remaining = get_cell_value_as_float(cellRangeRemaining);
      lv_label_set_text_fmt(label_total, "Total Spent: %s", format_money(*total));
      lv_label_set_text_fmt(label_remaining, "Remaining: %s", format_money(remaining));
      update_archive_totals();
    } else {
      Serial.println("GSheet not ready. Could not update values.");
    }  
  }

  lv_textarea_set_text(ta, "");
  
  // Close keyboard
  if(keypad) {
    lv_obj_del(keypad);
    keypad = NULL;
  }
}


// Displays numerical values to 2 dp
const char* format_money(float amount) {
    static char buf[20];          
    dtostrf(amount, 6, 2, buf);    
    return buf;
}


// READ spreadsheet
float get_cell_value_as_float(const String& cellRange) {
    FirebaseJson response;

    bool success = GSheet.values.get(&response, spreadsheetId, cellRange);
    response.toString(Serial, true); // print response for debug
    Serial.println();
    
    if (!success) {
        Serial.println("Failed to get cell value: " + GSheet.errorReason());
        return NAN; // Not a Number if failed
    }

    // Parse the value from JSON
    FirebaseJsonData jsonData; // object that keeps the deserializing result
    response.get(jsonData, "values/[0]/[0]");
    if (jsonData.success) {
        return jsonData.to<float>();
    } else {
        Serial.println("Failed to parse cell value");
        return NAN;
    }
}

// UPDATE spreadsheet
bool update_cell_with_float(float value, const String& cellRange) {
    FirebaseJson valueRange;
    FirebaseJson response;

    // Set range and write value
    valueRange.add("range", cellRange);
    valueRange.add("majorDimension", "ROWS");
    valueRange.set("values/[0]/[0]", value);

    bool success = GSheet.values.update(&response, spreadsheetId, cellRange, &valueRange);
    response.toString(Serial, true);  // print response for debug
    Serial.println();

    if (!success) {
        Serial.println("Failed to update cell: " + GSheet.errorReason());
        return false;
    }

    return true;
}

// APPEND spreadsheet values to archive
bool append_totals_to_sheet2(float groceries, float leisure, float travel, float other, const String& dateString) {
    FirebaseJson response;
    FirebaseJson valueRange;

    valueRange.add("majorDimension", "ROWS");
    valueRange.set("values/[0]/[0]", dateString);
    valueRange.set("values/[0]/[1]", groceries);
    valueRange.set("values/[0]/[2]", leisure);
    valueRange.set("values/[0]/[3]", travel);
    valueRange.set("values/[0]/[4]", other);

    bool success = GSheet.values.append(
        &response,
        spreadsheetId,       
        "Sheet2!A2",    
        &valueRange
    );
    response.toString(Serial, true); // print response for debug
    Serial.println();
    
    if (!success) {
      Serial.println("Failed to append totals: " + GSheet.errorReason());
      return false;
    }

    return true;
}

// Resets expense totals to 0
void reset_category(float* total, const String& cellRangeUpdate, const String& cellRangeRemaining, lv_obj_t* label_total, lv_obj_t* label_remaining) {
  *total = 0.00;
  update_cell_with_float(*total, cellRangeUpdate);
  float remaining = get_cell_value_as_float(cellRangeRemaining);
  lv_label_set_text_fmt(label_total, "Total Spent: %s", format_money(*total));
  lv_label_set_text_fmt(label_remaining, "Remaining: %s", format_money(remaining));
}


String get_current_date() {
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) {
        Serial.println("Failed to obtain time");
        return "Unknown";
    }

    char buffer[11];
    strftime(buffer, sizeof(buffer), "%d/%m/%Y", &timeinfo);
    return String(buffer);
}


// Reupdates labels on archive screen
void update_archive_totals(void) {
  lv_label_set_text_fmt(archive_groceries, "Total Spent Groceries: %s", format_money(total_spent_groceries));
  lv_label_set_text_fmt(archive_leisure, "Total Spent Leisure: %s", format_money(total_spent_leisure)); 
  lv_label_set_text_fmt(archive_travel, "Total Spent Travel: %s", format_money(total_spent_travel));
  lv_label_set_text_fmt(archive_other, "Total Spent Other: %s", format_money(total_spent_other));
}

void tokenStatusCallback(TokenInfo info){
    if (info.status == token_status_error){
        Serial.printf("Token info: type = %s, status = %s\n", GSheet.getTokenType(info).c_str(), GSheet.getTokenStatus(info).c_str());
        Serial.printf("Token error: %s\n", GSheet.getTokenError(info).c_str());
    }
    else{
        Serial.printf("Token info: type = %s, status = %s\n", GSheet.getTokenType(info).c_str(), GSheet.getTokenStatus(info).c_str());
    }
}

void setup() {
  Serial.begin( 115200 ); /* prepare for possible serial debug */
  
  u.begin();
  u.tftp = (void*) &tft;
  u.tsp->setRotation(1);
  u.backlight(true);

  String LVGL_Arduino = "Hello Arduino! ";
  LVGL_Arduino +=
    String('V') + lv_version_major() + "." +
    lv_version_minor() + "." + lv_version_patch();

  Serial.println( LVGL_Arduino );
  Serial.println( "I am LVGL_Arduino" );


  lv_init();

#if LV_USE_LOG != 0
  lv_log_register_print_cb( my_print ); /* register print function for debugging */
#endif

  tft.begin();      /* TFT init */
  tft.setRotation( 1 ); /* Landscape orientation */

  /*Set the touchscreen calibration data,
   the actual data for your display can be acquired using
   the Generic -> Touch_calibrate example from the TFT_eSPI library*/
  uint16_t calData[5] = { 347, 3549, 419, 3352, 5 };
  tft.setTouch( calData );

  lv_disp_draw_buf_init( &draw_buf, buf, NULL, screenWidth * 10 );

  /*Initialize the display*/
  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init( &disp_drv );
  /*Change the following line to your display resolution*/
  disp_drv.hor_res = screenWidth;
  disp_drv.ver_res = screenHeight;
  disp_drv.flush_cb = my_disp_flush;
  disp_drv.draw_buf = &draw_buf;
  lv_disp_drv_register( &disp_drv );

  /*Initialize the (dummy) input device driver*/
  static lv_indev_drv_t indev_drv;
  lv_indev_drv_init( &indev_drv );
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = my_touchpad_read;
  lv_indev_drv_register( &indev_drv );


  // Connect to WIFI
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to Wi-Fi");
  while(WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("WiFi Connected with IP: ");
  Serial.println(WiFi.localIP());
  Serial.println();

  const long gmtOffset_sec = 0; 
  const int daylightOffset_sec = 3600;
  configTime(gmtOffset_sec, daylightOffset_sec, "pool.ntp.org");

  // Google Sheets setup
  // Set the callback for Google API access token generation status (for debug only)
  GSheet.setTokenCallback(tokenStatusCallback);

  // Set the seconds to refresh the auth token before expire (60 to 3540, default is 300 seconds)
  GSheet.setPrerefreshSeconds(10 * 60);

  // Begin the access token generation for Google API authentication
  GSheet.begin(CLIENT_EMAIL, PROJECT_ID, PRIVATE_KEY);

  if (GSheet.ready()) {
    budget_groceries = get_cell_value_as_float("Sheet1!B2");
    budget_leisure = get_cell_value_as_float("Sheet1!C2");
    budget_travel = get_cell_value_as_float("Sheet1!D2");
    budget_other = get_cell_value_as_float("Sheet1!E2");

    Serial.print("Groceries Budget: "); 
    Serial.println(budget_groceries);
    Serial.print("Leisure Budget: ");   
    Serial.println(budget_leisure);
    Serial.print("Travel Budget: ");    
    Serial.println(budget_travel);
    Serial.print("Other Budget: ");    
    Serial.println(budget_other);
  } else {
    Serial.println("GSheet not ready. Could not fetch budget values.");
  }


  if (GSheet.ready()) {
    total_spent_groceries = get_cell_value_as_float("Sheet1!B3");
    total_spent_leisure = get_cell_value_as_float("Sheet1!C3");
    total_spent_travel = get_cell_value_as_float("Sheet1!D3");
    total_spent_other = get_cell_value_as_float("Sheet1!E3");

    Serial.print("Groceries Spent: "); 
    Serial.println(total_spent_groceries);
    Serial.print("Leisure Spent: ");   
    Serial.println(total_spent_leisure);
    Serial.print("Travel Spent: ");    
    Serial.println(total_spent_travel);
    Serial.print("Other Spent: ");    
    Serial.println(total_spent_other);
  } else {
    Serial.println("GSheet not ready. Could not fetch spent values.");
  }

  if (GSheet.ready()) {
    remaining_groceries = get_cell_value_as_float("Sheet1!B4");
    remaining_leisure = get_cell_value_as_float("Sheet1!C4");
    remaining_travel = get_cell_value_as_float("Sheet1!D4");
    remaining_other = get_cell_value_as_float("Sheet1!E4");

    Serial.print("Remaining Groceries: "); 
    Serial.println(remaining_groceries);
    Serial.print("Remaining Leisure: ");   
    Serial.println(remaining_leisure);
    Serial.print("Remaining Travel: ");    
    Serial.println(remaining_travel);
    Serial.print("Remaining Other: ");    
    Serial.println(remaining_other);
  } else {
    Serial.println("GSheet not ready. Could not fetch remaining values.");
  }

  createGUI(); 
  
  Serial.println( "Setup done" );
}

void loop() {
  bool ready = GSheet.ready();
  lv_timer_handler();
  delay(5);

  static bool last_btn1 = HIGH;
  static bool last_btn2 = HIGH;
  static bool last_btn3 = HIGH;

  bool btn1 = u.button1();
  bool btn2 = u.button2();
  bool btn3 = u.button3();

  if (btn1 == LOW && last_btn1 == HIGH) {
    current_screen = (current_screen + 4) % 5;  // up
    lv_scr_load(screens[current_screen]);
  }
  if (btn2 == LOW && last_btn2 == HIGH) {
    current_screen = (current_screen + 1) % 5;  // down
    lv_scr_load(screens[current_screen]);
  }
  if (btn3 == LOW && last_btn3 == HIGH) { // add
    add_amount_from_button();
  }

  last_btn1 = btn1;
  last_btn2 = btn2;
  last_btn3 = btn3;
}
