
// includes
#include "GPSDog.h"


GPSDog::GPSDog()
{
    m_isInit            = false;
    m_alarmOverload     = false;
    m_gpsFix            = false;

    m_nextAlarmSMS      ^= m_nextAlarmSMS;
    m_alarmStartTime    ^= m_alarmStartTime;
}
        
void GPSDog::initialize(char *smsNum, uint8_t smsNumSize, char *smsTxt, uint8_t smsTxtSize, void (*cbSendSMS)(), void (*cbCheckSMS)(), void (*cbReloadSMS)(), void (*cbReceiveGPS)())
{
    // init data
    m_number            = smsNum;
    m_numberSize        = smsNumSize;
    m_message           = smsTxt;
    m_messageSize       = smsTxtSize;
    

    // callbacks
    cb_sendSMS          = cbSendSMS;
    cb_checkNewSMS      = cbCheckSMS;
    cb_reloadSMS        = cbReloadSMS;
    cb_receiveGPS       = cbReceiveGPS;

    // set init flag
    m_isInit            = true;
}

void GPSDog::mainProcessing()
{
    // check is init
    if (!m_isInit) {
        return;
    }

    while (1) {

        ////
        // if Alarm mode is on
        if (this->isModeOn(GPSDOG_MODE_ALARM) && m_gpsFix) {
            // Time to new send a alarm
            if (m_nextAlarmSMS <= millis() && !m_alarmOverload) {
                this->sendAlarmSMS();
            }
        }

        ////
        // processing GPS data
        if (!m_gpsFix && millis() >= GPSDOG_WAIT_GPSFIX) {
            // wait time after boot is ok, position is fix
            m_gpsFix = true; 

            // if GPSDog wait for watching out
            if (this->isModeOn(GPSDOG_MODE_DOWATCH)) {
                this->doWatching();

                // Reset state & save
                this->setMode(GPSDOG_MODE_DOWATCH, false);
                this->writeConfig();

                // send notify that modus is on
                this->sendNotifySMS();
            }
        }

        // update position
        this->cb_receiveGPS();

        ////
        // process command sms
        this->cb_checkNewSMS();

        ////
        // wait for next process
        delay(GPSDOG_WAIT_PROCESSING);

        ////
        // Check Alarm Overloaded
        if (m_alarmOverload) {
            if (m_alarmStartTime > millis()) {
                m_alarmOverload = false;
            }
        }
    }
}

void GPSDog::processIncomingSMS()
{
    char    *smsCmd;
    uint8_t count;
    bool    legalNum;

    // If Protect mode active
    legalNum = this->foundNumberInStore(m_number);

    ////
    // Parse Data
    count   = this->parseSMSMessage();
    smsCmd  = this->getSMSCommand();

    ////
    // Find Master command

    // STATUS
    if (strncmp_P(smsCmd, GPSDOG_TXT_STATUS, 6) == 0 && count == 0) {
        // check modus for legal number or protected is off
        if (legalNum || !this->isModeOn(GPSDOG_MODE_PROTECT)) {
            this->createStatusSMS();
        }
        // No Answer
        else {
            return;
        }
    }
    // INIT pw number sign ON/OFF
    else if (strncmp_P(smsCmd, GPSDOG_TXT_INIT, 4) == 0 && count == 4) {
        this->readInitFromSMS();
    }
    // RESET pw
    else if (strncmp_P(smsCmd, GPSDOG_TXT_RESET, 5) == 0 && count == 1) {
        this->readResetFromSMS();
    }
    // STORE idx ADD number sign ON/OFF
    // STORE idx DEL
    // STORE idx SHOW
    else if (legalNum && strncmp_P(smsCmd, GPSDOG_TXT_STORE, 5) == 0 && count >= 2) {
        this->readStoreFromSMS();
    }
    // SET INTERVAL min
    // SET FORWARD idx
    // SET GEOFIX VAL
    // SET UNIT KMH/MPH
    else if (legalNum && strncmp_P(smsCmd, GPSDOG_TXT_SET, 3) == 0 && count == 2) {
        this->readSetFromSMS();
    }
    // VERSION
    else if (legalNum && strncmp_P(smsCmd, GPSDOG_TXT_VERSION, 7) == 0 && count == 0) {
        this->createDefaultSMS(GPSDOG_OPT_SMS_VERSION);
    }
    // ALARM ON/OFF/?
    else if (legalNum && strncmp_P(smsCmd, GPSDOG_TXT_ALARM, 5) == 0 && count == 1) {
        this->readModeFromSMS(GPSDOG_MODE_ALARM);
    }
    // WATCH ON/OFF/?
    else if (legalNum && strncmp_P(smsCmd, GPSDOG_TXT_WATCH, 5) == 0 && count == 1) {
        this->readModeFromSMS(GPSDOG_MODE_WATCH);
    }
    // PROTECT ON/OFF/?
    else if (legalNum && strncmp_P(smsCmd, GPSDOG_TXT_PROTECT, 7) == 0 && count == 1) {
        this->readModeFromSMS(GPSDOG_MODE_PROTECT);
    }
    // FORWARD ON/OFF/?
    else if (legalNum && strncmp_P(smsCmd, GPSDOG_TXT_FORWARD, 7) == 0 && count == 1) {
        this->readModeFromSMS(GPSDOG_MODE_FORWARD);
    }
    // STOP
    else if (legalNum && strncmp_P(smsCmd, GPSDOG_TXT_STOP, 4) == 0 && count == 0) {
        // Stop ALARM & WATCH
        this->setMode(GPSDOG_MODE_ALARM, false);
        this->setMode(GPSDOG_MODE_WATCH, false);

        this->writeConfig();
        this->createDefaultSMS(GPSDOG_OPT_SMS_DONE);
    }
    // Unknown command
    else {
        ////
        // Is forward active, do it!
        if (!legalNum && this->isModeOn(GPSDOG_MODE_FORWARD)) {
            // restore original message
            this->cb_reloadSMS();

            // replace number
            if (!this->setNumber(m_numbers[this->getForwardIdx()])) {
                return;
            }
        }
        // Not unswer to a unknown number
        else if (!legalNum) {
            return;
        }
        // command unknown
        else {
            this->createDefaultSMS(GPSDOG_OPT_SMS_UNKNOWN);
        }
    }

    ////
    // Send Answer
    this->cb_sendSMS();
}

void GPSDog::updateGPSData(double latitude, double longitude, double speed, char *date, char *time)
{
    ////
    // Copy new Data
    m_latitude  = latitude;
    m_longitude = longitude;

    // KMH
    if (this->getUnit() == GPSDOG_UNIT_KMH) {
        m_speed     = speed * 1.6;
    }
    // MPH
    else {
        m_speed     = speed;
    }

    this->copyDateTime(date, time);

    ////
    // GPSDog Watch ON / Check of state change and position is fix
    if (this->isModeOn(GPSDOG_MODE_WATCH) && !this->isModeOn(GPSDOG_MODE_ALARM) && m_gpsFix) {

        // Position change
        if (!this->cmpGeoData(this->getStoreLatitude(), latitude, this->getStoreGeoFix()) || !this->cmpGeoData(this->getStoreLongitude(), longitude, this->getStoreGeoFix())) {
            
            ////
            // set Alarm
            this->setMode(GPSDOG_MODE_ALARM, true);
            this->writeConfig();
            this->sendAlarmSMS();
        }
    }
}

void GPSDog::sendNotifySMS()
{
    // find numbers where have a active notify
    for (uint8_t i = 0; i < GPSDOG_CONF_NUMBER_STORE; i++) {

        // Notify is On
        if (this->isAlarmNotifyOn(i)) {
            // Send Status SMS
            if (this->setNumber(m_numbers[i])) {
                this->cb_sendSMS();
            }
        }
    }
}

void GPSDog::sendAlarmSMS()
{
    // calc next alarm SMS
    this->calcNextAlarm();

    // generate Status SMS Text
    this->createStatusSMS();

    // send SMS
    this->sendNotifySMS();
}


void GPSDog::calcNextAlarm()
{
    uint32_t interVal   = static_cast<uint32_t>(this->getAlarmInterval());
    
    // calc milliseconds
    interVal *= 60000;

    m_alarmStartTime    = millis();
    m_nextAlarmSMS      = m_alarmStartTime + interVal;

    // overloaded
    if (m_nextAlarmSMS < m_alarmStartTime) {
       m_alarmOverload = true;
    }
    else {
       m_alarmOverload = false;
    }
}

void GPSDog::createStatusSMS()
{
    char lat[12];
    char lon[12];
    char speed[7];
    char stat[7];

    // init buffer sms text
    if (!this->cleanSMS()) {
        return;
    }

    // convert value
    this->getLatitude(lat, 12);
    this->getLongitude(lon, 12);
    this->getSpeed(speed, 7);

    ////
    // Copy status
    memset(stat, 0x00, 7);
    if (this->isModeOn(GPSDOG_MODE_ALARM)) {
        strncpy_P(stat, GPSDOG_TXT_ALARM, 6);
    }
    else if (this->isModeOn(GPSDOG_MODE_WATCH)) {
        strncpy_P(stat, GPSDOG_TXT_WATCH, 6);
    }
    else {
        strncpy_P(stat, GPSDOG_TXT_STATUS, 6);
    }

    ////
    // create SMS Text
    snprintf_P(m_message, m_messageSize -1, GPSDOG_SMS_STATUS, stat, lat, lon, speed, m_date, m_time, lat, lon);
}

void GPSDog::createDefaultSMS(uint8_t msgOpt)
{
    // init buffer sms text
    if (!this->cleanSMS()) {
        return;
    }

    switch (msgOpt) {
        case GPSDOG_OPT_SMS_DONE :
            strncpy_P(m_message, GPSDOG_SMS_DONE, m_messageSize -1);
            break;
        case GPSDOG_OPT_SMS_ERROR :
            strncpy_P(m_message, GPSDOG_SMS_SYSERROR, m_messageSize -1);
            break;
        case GPSDOG_OPT_SMS_UNKNOWN :
            strncpy_P(m_message, GPSDOG_SMS_UNKNOWN, m_messageSize -1);
            break;
        case GPSDOG_OPT_SMS_INIT :
            strncpy_P(m_message, GPSDOG_SMS_INIT, m_messageSize -1);
            break;
        case GPSDOG_OPT_SMS_VERSION :
            strncpy_P(m_message, GPSDOG_SMS_VERSION, m_messageSize -1);
            break;
        case GPSDOG_OPT_SMS_WATCH :
            strncpy_P(m_message, GPSDOG_SMS_WATCH, m_messageSize -1);
            break;
    }
}

void GPSDog::createModeStateSMS(uint8_t mode)
{
    char    modeName[8];
    char    onOff[4];

    // Init mode name
    memset(modeName, 0x00, 8);
    strncpy(modeName, this->getParseElement(0), 7);

    // init notify txt
    this->textOnOff(onOff, 4, this->isModeOn(mode));

    // init buffer sms text
    if (!this->cleanSMS()) {
        return;
    }

    // generate text
    snprintf_P(m_message, m_messageSize -1, GPSDOG_SMS_MODE, modeName, onOff);
}

bool GPSDog::parseOnOff(uint8_t idx)
{
    char *onOff = this->getParseElementUpper(idx);

    // ON
    if (strncmp_P(onOff, GPSDOG_TXT_ON, 2) == 0) {
        return true;
    }

    return false;
}

void GPSDog::textOnOff(char *buffer, uint8_t size, bool onOff)
{
    // init buffer
    memset(buffer, 0x00, size);

    // Generate text for alarm Notify
    if (onOff) {
        strncpy_P(buffer, GPSDOG_TXT_ON, size -1);
    }
    else {
        strncpy_P(buffer, GPSDOG_TXT_OFF, size -1);
    }
}

void GPSDog::readModeFromSMS(uint8_t mode)
{
    char *opt = this->getParseElement(1);

    ////
    // Ask status an give a answer
    if (opt[0] == GPSDOG_CHAR_ASK) {
        this->createModeStateSMS(mode);
        return;
    }

    ////
    // Read state
    bool onOff = this->parseOnOff(1);

    ////
    // Set mode
    // Watch Mode 
    if (onOff && mode == GPSDOG_MODE_WATCH) {
        this->doWatching();
    }
    // Default
    else {
        this->setMode(mode, onOff);
        this->createDefaultSMS(GPSDOG_OPT_SMS_DONE);
    }

    // save
    this->writeConfig();
}

void GPSDog::readInitFromSMS()
{
    char    *pw         = this->getParseElement(1);
    char    *number     = this->getParseElement(2);
    uint8_t sign        = atoi(this->getParseElement(3));
    bool    alarmNotify = this->parseOnOff(4);

    // allready init
    if (this->isModeOn(GPSDOG_MODE_INIT)) {
        goto Error;
    }

    // set number to first store
    if (!this->addNumberWithNotify(0, number, sign, alarmNotify)) {
        goto Error;
    }

    // write passwort
    if (!this->setPasswordInit(pw)) {
        goto Error;
    }

    // write config
    this->writeConfig();
    this->createDefaultSMS(GPSDOG_OPT_SMS_INIT);
    return;

Error:
    this->readConfig();
    this->createDefaultSMS(GPSDOG_OPT_SMS_ERROR);
    return;
}

void GPSDog::readResetFromSMS()
{
    char *pw = this->getParseElement(1);

    // init mode is set and passsword is okay
    if (!this->isModeOn(GPSDOG_MODE_INIT) || !this->checkPassword(pw)) {
        this->createDefaultSMS(GPSDOG_OPT_SMS_ERROR);
        return;
    }

    // reset config
    this->cleanConfig();

    // save config
    this->writeConfig();

    // end
    this->createDefaultSMS(GPSDOG_OPT_SMS_DONE);
}

void GPSDog::readSetFromSMS()
{
    char    *cmd    = this->getParseElementUpper(1);
    char    *opt    = this->getParseElement(2);

    // SET INTERVAL min
    if (strncmp_P(cmd, GPSDOG_TXT_INTERVAL, 8) == 0) {
        this->setAlarmInterval(atoi(opt));
    }
    // SET FORWARD idx
    else if (strncmp_P(cmd, GPSDOG_TXT_FORWARD, 7) == 0) {
        this->setForwardIdx(atoi(opt) -1);
    }
    // SET GEOFIX VAL
    else if (strncmp_P(cmd, GPSDOG_TXT_GEOFIX, 6) == 0) {
        this->setStoreGeoFix(atof(opt));
    }
    // SET UNIT KMH/MPH
    else if (strncmp_P(cmd, GPSDOG_TXT_UNIT, 4) == 0) {
        opt    = this->getParseElementUpper(2);

        // KMH
        if (strncmp_P(opt, GPSDOG_TXT_KMH, 3) == 0) {
            this->setUnit(GPSDOG_UNIT_KMH);
        }
        // MPH
        else if (strncmp_P(opt, GPSDOG_TXT_MPH, 3) == 0) {
            this->setUnit(GPSDOG_UNIT_MPH);
        }
        // ERROR
        else {
            goto Error;
        }
    }
    // ERROR
    else {
        goto Error;
    }

    this->writeConfig();
    this->createDefaultSMS(GPSDOG_OPT_SMS_DONE);
    return;

Error:
    this->createDefaultSMS(GPSDOG_OPT_SMS_ERROR);
    return;
}

void GPSDog::readStoreFromSMS()
{
    uint8_t idx     = atoi(this->getParseElement(1)) -1;
    char    *cmd    = this->getParseElementUpper(2);

    // Store number in range
    if (idx >= GPSDOG_CONF_NUMBER_STORE || idx < 0x00) {
        goto Error;
    }

    // STORE num ADD number sign ON/OFF
    if (strncmp_P(cmd, GPSDOG_TXT_ADD, 3) == 0 && m_lastParamCount == 5) {
        char    *number = this->getParseElement(3);
        uint8_t sign    = atoi(this->getParseElement(4));
        bool    notify  = this->parseOnOff(5);
    
        // Ckeck number is set
        if (!this->addNumberWithNotify(idx, number, sign, notify)) {
            goto Error;
        }
       
        // end
        goto Done;
    }
    // STORE num DEL
    else if (strncmp_P(cmd, GPSDOG_TXT_DEL, 3) == 0 && m_lastParamCount == 2) {
        // clean number
        memset(m_numbers[idx], 0x00, GPSDOG_CONF_NUM_SIZE +1);
        this->setAlarmNotify(idx, false);

        // end
        goto Done;
    }
    // STORE num SHOW
    else if (strncmp_P(cmd, GPSDOG_TXT_SHOW, 4) == 0 && m_lastParamCount == 2) {
        char    onOff[4];
        uint8_t sign        = this->getSignNumber(idx);

        // init notify txt
        this->textOnOff(onOff, 4, this->isAlarmNotifyOn(idx));
        
        // write
        snprintf_P(m_message, m_messageSize -1, GPSDOG_SMS_STORESHOW, m_numbers[idx], sign, onOff);
        return;
    }

Error:
    this->readConfig();
    this->createDefaultSMS(GPSDOG_OPT_SMS_ERROR);
    return;

Done:
    this->writeConfig();
    this->createDefaultSMS(GPSDOG_OPT_SMS_DONE);
    return;
}

void GPSDog::doWatching()
{
    // Postion is fix / save this GPS Data
    if (m_gpsFix) {
        this->setStoreLatitude(m_latitude);
        this->setStoreLongitude(m_longitude);

        this->setMode(GPSDOG_MODE_WATCH, true);
        this->createDefaultSMS(GPSDOG_OPT_SMS_WATCH);
    }
    // if GPS is not Fix, you can start watch modus later
    else {
        uint32_t timeDone = GPSDOG_WAIT_GPSFIX - millis();

        // Calc in sec
        if (timeDone < 1000) {
            timeDone = 1;
        }
        else {
            timeDone /= 1000;
        }

        // generate message
        if (this->cleanSMS()) {
            snprintf_P(m_message, m_messageSize -1, GPSDOG_SMS_GPSFIX, timeDone);
        }

        this->setMode(GPSDOG_MODE_DOWATCH, true);
        return;
    }
}

// vim: set sts=4 sw=4 ts=4 et:
