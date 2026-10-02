/*******************************************************************************
 * Copyright (c) 2026, Oak Ridge National Laboratory (Joseph Olatt,            *
 *  Joel Asiamah, Sam Hollifield)                                              *
 *                                                                             *
 * Redistribution and use in source and binary forms, with or without          *
 * modification, are permitted provided that the following conditions are      *
 * met:                                                                        *
 *                                                                             *
 *   1. Redistributions of source code must retain the above copyright notice, *
 *      this list of conditions and the following disclaimer.                  *
 *   2. Redistributions in binary form must reproduce the above copyright      *
 *      notice, this list of conditions and the following disclaimer in the    *
 *      documentation and/or other materials provided with the distribution.   *
 *                                                                             *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" *
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE   *
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE  *
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE    *
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR         *
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF        *
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS    *
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN     *
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)     *
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE  *
 * POSSIBILITY OF SUCH DAMAGE.                                                 *
 ******************************************************************************/
/*******************************************************************************
 * eldsentry.ino                                                               *
 *  This code is written to run on a Teensy 4.1 microcontroller. The code      *
 *  performs the function of a gateway/firewall. It supports 2 CAN             *
 *  interfaces. One interface is connected to the diagnostic port (Pins 22,    *
 *  23) and the other is connected to the ELD (Pins 0, 1). All traffic from    *
 *  the diagnostic port is passed to the ELD without any filtering. Only       *
 *  specific requests are allowed to flow from the ELD to the diagnostic port. *
 *                                                                             *
 *  The code also writes all CAN messages it reads from the diagnostic port    *
 *  to an SD card. Each file will FILE_MSG_MAX messages. Each CAN message is   *
 *  CAN_MSG_LEN bytes long. When any given file reaches FILE_MSG_MAX messages, *
 *  a new file is started. The new file will be named candump-<number>.log.    *
 *  <number> is tracked by the file defined by, COUNTER_FILE.                  *                          *                                                                             *
 *  A blank SD card, with default formatting (FAT32), or formatted with        *
 *  "format_sd" from sdcard.org/downloads is to be used. The number of files   *
 *  to create, etc. are automatically computed by the code.                    *
 *                                                                             *
 *  The setup that this code runs on comprises of the following components:    *
 *    1. Teensy 4.1                                                            *
 *    2. Waveshare or Adafruit CAN Bus transceiver                             *
 *    3. 12 volt to 5 volt DC converter (Buck or Linear)                       *
 *    4. Breadboard                                                            *
 *    5. Male and Female J1939 Pigtails                                        *
 *                                                                             *
 * For more information, see: https://eldsentry.ornl.gov
 *******************************************************************************/
#include <FlexCAN_T4.h>
#include <SD.h>
#include <math.h>

#define COUNTER_FILE    "counter.txt"
#define VERSION         "20260818.00"
#define VERSION_FILE    "version.txt"
#define FILE_MSG_MAX    500000

#define CAN_MSG_MAX_LEN 50
#define CAN_BIT_RATE    500000
#define BLOCK_SZ        1024

const int chipSelect = BUILTIN_SDCARD;

SdVolume  volume;
Sd2Card   card;

File      dataFile;
File      cntFile;
File      verFile;

uint32_t  msg_ctr;
uint32_t  fnum;

uint8_t   buf[BLOCK_SZ];
uint8_t   line[CAN_MSG_MAX_LEN];
char      dat_file[24];
int32_t   dfile_sz;

uint64_t  sd_sz;
uint8_t   eld_src_add;

uint64_t  sd_avail_sz;
uint8_t   line_ctr;

CAN_message_t msg;
unsigned long elapsed_millis;

FlexCAN_T4<CAN1, RX_SIZE_256, TX_SIZE_256> can1;
FlexCAN_T4<CAN2, RX_SIZE_256, TX_SIZE_256> can2;

const uint32_t allwd_req_pgns[] = {54016, 60928, 65216, 65242, 65244, 65253, 
                                   65257, 65259, 65260};

void
setup()
{

    pinMode(LED_BUILTIN, OUTPUT);
    Serial.begin(115200);
    //while (!Serial);               // Wait for serial terminal

    Serial.println("Going to initialize sdcard ...");

    /* Initialize SD card */
    if (!SD.begin(chipSelect))
    {
        Serial.println("SD card initialization failed, or card not present!");
        return;
    }

    Serial.println("SD Card initialized ...");

    /* Get size of SD card */
    sd_sz = getSDCardSize();
    Serial.print("SD card size (Bytes): ");
    Serial.println(sd_sz);

    /* List files on SD card */
    listFiles();

    /* Write version of firmware to version.txt on SD card */
    memset(buf, '\0', sizeof(buf));
    snprintf((char *) buf, sizeof(buf), "ELDSentry version: %s\n", VERSION);
    verFile =  SD.open(VERSION_FILE, FILE_WRITE_BEGIN);
    if (verFile)
    {
        verFile.write((const uint8_t *) buf, strlen((const char *) buf));
        verFile.flush();
        verFile.close();
    }
    else
    {
        Serial.println("ERROR - Failed to write version to version.txt!");
    }

    /* Initialize both CAN interfaces */
    can1.begin();
    can1.setBaudRate(CAN_BIT_RATE);
    Serial.println("CAN1 initialized ...");

    can2.begin();
    can2.setBaudRate(CAN_BIT_RATE);
    Serial.println("CAN2 initialized ...");

    /* Initialize buf */
    memset(buf, '\0', sizeof(buf));

    /* Initalize line buffer */
    memset(line, '\0', sizeof(line));

    Serial.println("Initialized buffers ...");

    /* Initialize counters */
    msg_ctr  = 0;
    line_ctr = 0;

    /* Initialize ELD source address to the Null Address 254 (0xFE) */
    eld_src_add = 254;

    /* Sleep 1 second before starting */
    delay(1000);
}

void 
loop()
{
    if (msg_ctr >= FILE_MSG_MAX)
    {
        dataFile.flush();
        dataFile.close();

        msg_ctr = 0;
    }

    if (msg_ctr == 0)
    {
        sd_avail_sz = getSDAvailSize();

        if (sd_avail_sz >= (FILE_MSG_MAX * CAN_MSG_MAX_LEN))
        {
            /* Write FILE_MSG_MAX messages to file, if there is space 
             * available (~21000000 bytes) on SD card to write another file.
             */
            ;
        }
        else
        {
            Serial.print("Insufficient available space: ");
            Serial.println(sd_avail_sz);
            blinkComplete();
        }

        /* Loop around to ensure that the new file does not exist */
        while (1)
        {
            /* Increment file counter number */
            if (incCounterFile())
            {
                Serial.print("File num: ");
                Serial.println(getCounterFileVal());
            }
            else
            {
                Serial.println("ERROR - incCounterFile(): failed!");
                blinkComplete();
            }
        
            /* Get the file counter value */
            fnum = getCounterFileVal();
    
            /* Build output file name */
            memset(dat_file, '\0', sizeof(dat_file));
            snprintf(dat_file, sizeof(dat_file), "candump-%lu.log", fnum);

            if (SD.exists(dat_file))
            {
                /* Do nothing. While loop will continue until a non-existent
                 * file name is created.
                 */
                ;
            }
            else
            {
                break;
            }
        }

        Serial.print("dat_file: ");
        Serial.println(dat_file);
        dataFile =  SD.open(dat_file, FILE_WRITE_BEGIN);
    
        Serial.println("Opened data file ...");
    }

    while (true)
    {
        if (can1.read(msg))
        {
            /* Turn LED ON */
            digitalWrite(LED_BUILTIN, HIGH);

            /* Write messages block to file if reached limit */
            if (line_ctr > (int) (BLOCK_SZ / CAN_MSG_MAX_LEN))
            {
                dataFile.write((const uint8_t *) buf, strlen((const char *) buf));

                /* Flush to disk */
                dataFile.flush();

                line_ctr = 0;
                memset(buf, '\0', BLOCK_SZ);
            }

            /* Initialize line buffer */
            memset(line, '\0', sizeof(line));

            /* Capture elapsed time */
            elapsed_millis = millis();

            /* Write message to can2 interface */
            can2.write(msg);

            /* Write message to line buffer */
            snprintf((char *) line, CAN_MSG_MAX_LEN, 
                     "(%0.3f) can0 %08lX#%02X%02X%02X%02X%02X%02X%02X%02X\r\n",
                     (elapsed_millis * powf(10, -3)),
                     msg.id,
                     msg.buf[0],
                     msg.buf[1],
                     msg.buf[2],
                     msg.buf[3],
                     msg.buf[4],
                     msg.buf[5],
                     msg.buf[6],
                     msg.buf[7]);

            /* Append contents of line buffer to block of CAN messages */
            strncat((char *) buf, (const char *) line, CAN_MSG_MAX_LEN);

            /* Increment line  & msg counters */
            line_ctr++;
            msg_ctr++;

            /* Turn LED OFF */
            digitalWrite(LED_BUILTIN, LOW);
            
            /* Break out of while loop to increment frame count */
            break;
        }
        else if (can2.read(msg))
        {
            /* These are the message IDs sent from Omnitracs ELD:
             *  18EA00A0
             *  18EA0FA0
             *  18EAFFA0
             *  18EEFFA0
             *  18FEF3A0
             *  1CEC00A0
             *
             * i.e. 2nd byte is 0xEA, then it is a request message.
             * The payload will be something like:
             *   18EA00A0#ECFE00
             * the payload will be 3 bytes and it should be a PGN. The allowed
             * PGNs are defined in the global array allwd_req_pgns.
             *
             * If the 2nd byte is 0xEC, then it is a CTS (Clear To Send) TP.DT
             * message from the ELD, to whichever ECU, saying it is okay to
             * send data. This message needs to go through.
             *
             * All others will be dropped.
             */
            if (isPassMsg(&msg))
            {
                /* Write message to can1 interface */
                can1.write(msg);

                memset(line, '\0', sizeof(line));
                snprintf((char *) line, CAN_MSG_MAX_LEN, "%08lX Allowed!\r\n", msg.id);
                Serial.println((char *) line);
            }
            else
            {
                memset(line, '\0', sizeof(line));
                snprintf((char *) line, CAN_MSG_MAX_LEN, 
                         "%08lX#%02X%02X%02X%02X%02X%02X%02X%02X DROPPED!\r\n",
                         msg.id,
                         msg.buf[0],
                         msg.buf[1],
                         msg.buf[2],
                         msg.buf[3],
                         msg.buf[4],
                         msg.buf[5],
                         msg.buf[6],
                         msg.buf[7]);
                Serial.println((char *) line);
            }
        }
        else
        {
            /* Sleep 1 millisecond */
            delay(1);
            continue;
        }
    }
}


void
listFiles()
{
    File root;
    File entry;

    DateTimeFields dtf;

    root = SD.open("/");

    while (true)
    {
        entry = root.openNextFile();
        if (! entry)
        {
            break;
        }

        if (! entry.isDirectory())
        {
            Serial.print(entry.name());
            Serial.print("    ");
            Serial.print(entry.size());
            Serial.print("    ");

            if (entry.getModifyTime(dtf))
            {
                Serial.print(dtf.year + 1900);
                Serial.print("-");
                Serial.print(dtf.mon);
                Serial.print("-");
                Serial.print(dtf.mday);
                Serial.print(" ");
                Serial.print(dtf.hour);
                Serial.print(":");
                Serial.print(dtf.min);
            }
            Serial.println("");
        }
    }
}


int
getCounterFileVal()
{
    int rv = -1;
    uint32_t buf[2];

    cntFile = SD.open(COUNTER_FILE);
    if (cntFile)
    {
        memset(buf, '\0', 8);
        rv = cntFile.read(buf, 4);
        if (rv > 0)
        {
            fnum = buf[0];
        }

        cntFile.close();
        rv = fnum;
    }

    return rv;
}


bool
incCounterFile()
{
    bool rv = false;
    uint32_t buf[2];

    cntFile = SD.open(COUNTER_FILE);
    if (cntFile)
    {
        fnum = getCounterFileVal();
        cntFile.close();
        
        if (fnum == (uint32_t) -1)
        {
            /* File was empty */
            fnum = 1;
        }
        else
        {
            /* Increment the file number and write it back to file */
            fnum = fnum + 1;
        }
    }
    else
    {
        /* Initialize file number */
        fnum = 1;
    }

    /* Re-open file to write */
    cntFile = SD.open(COUNTER_FILE, FILE_WRITE_BEGIN);
    if (cntFile)
    {
        memset(buf, '\0', sizeof(buf));
        buf[0] = fnum;

        /* Write new value into file */
        cntFile.write(buf, 4);
        cntFile.flush();
        cntFile.close();

        rv = true;
    }
    else
    {
        Serial.println("Failed to re-open file!");
    }

    return rv;
}


void
blinkComplete()
{
    /* This routine will blink the LED as follows:
     *  1. Turn OFF for 1 second
     *  2. Turn ON for 1 second
     *  3. Turn OFF for 1 second
     *  4. Then, blink rapidy 3 times 
     */
    while (true)
    {
        delay(1000);
        for (int i = 0; i < 3; i++)
        {
            digitalWrite(LED_BUILTIN, HIGH);
            delay(250);
            digitalWrite(LED_BUILTIN, LOW);
            delay(250);
        }
    }
}


uint64_t
getSDAvailSize()
{
    uint64_t rv = -1;
    uint64_t tot_sz = 0;

    File root = SD.open("/");
    while (true)
    {
        File entry = root.openNextFile();
        if (! entry)
        {
            break;
        }

        if (entry.isDirectory())
        {
            /* There should not be any directories as all the files will be
             * at root level. So, close the file and get next file.
             */
            entry.close();
            continue;
        }

        tot_sz += entry.size();
    }

    if (tot_sz > 0)
    {
        rv = sd_sz - tot_sz;
    }

    return rv;
}


uint64_t 
getSDCardSize()
{
    uint64_t rv;

    if (! volume.init(card))
    {
        rv = -1;
    }

    /* Get number of blocks per cluster */
    rv = volume.blocksPerCluster();

    /* Multiply by total number of clusters */
    rv *= volume.clusterCount();

    /* Each block contains 512 bytes. So, to get size of SD card in bytes, we
     * have to multiply by 512
     */
     rv *= 512;

    return rv;
}


bool
isPassMsg(CAN_message_t * msg_ptr)
{
    uint32_t val;
    uint8_t b2;
    uint16_t pgn;

    /* First, check the CAN ID to see if it is one that is allowed to pass. 
     * If byte 2 value is less than 240, then we only need to allow 
     * following PGNs:
     *   59904 (0xEA00) - Request PGN
     *   60416 (0xEC00) - TP.CM.xx
     *   60928 (0xEE00) - Address claim by ELD
     */
    const uint8_t allwd_byte2s[] = {0xEA, 0xEC, 0xEE};

    /* If byte 2 of CAN ID >= 240, then we need to find value of byte 2 and 
     * byte 3. That value represents the PGN. Then only following PGNs will 
     * be allowed:
     *   65267 (0xFEF3) - latitude and longitude
     */
    const uint16_t allwd_byte23s[] = {0xFEF3};

    /* Calculate the lengths of arrays allwd_byte2s and allwd_byte23s */
    int ab2s_len = sizeof(allwd_byte2s) / sizeof(uint8_t);
    int ab23s_len = sizeof(allwd_byte23s) / sizeof(uint16_t);

    val = msg_ptr->id;
    val = val << 8;
    val = val >> 24;
    b2 = val;

    if (b2 < 240)
    {
        pgn = b2 << 8;
        if (pgn == 59904)
        {
             if (isAllowedReqPGN(msg_ptr->buf))
                return true;
             else
                return false;
        }
        else
        {
            /* Check if message is an address claim */
            if (pgn == 60928)
            {
                /* If so, and if eld_src_add is not set, extract and set it 
                 * now.
                 */
                if (eld_src_add == 254)
                {
                    eld_src_add = msg_ptr->id & 0x000000FF;
                    Serial.print("eld_src_add: ");
                    Serial.println(eld_src_add);
                }
                else
                {
                    /* Hmm. Looks like address claim was previously made as
                     * eld_src_add has a non-zero value. Let's see what address
                     * the ELD is trying to claim. If matches what is stored
                     * in eld_src_add, then we'll allow it to pass.
                     */
                    if ((msg_ptr->id & 0x000000FF) == eld_src_add)
                    {
                        return true;
                    }
                    else
                    {
                        /* Why would the ELD be trying to claim another 
                         * address?? Drop this message for now, until we 
                         * understand the reason for this message.
                         */
                        Serial.print((msg_ptr->id & 0x000000FF), HEX);
                        Serial.println(": Questionable source address claim");
                        return false;
                    }
                }

                /* Source addresses that ELDs are claiming should be greater
                 * than or equal to 128--preferrably 250. 250 is set aside for 
                 * off board diagnostic tool #2, which is what the ELD should 
                 * ideally use. 
                 *
                 * It appears that the spec allows for source addresses in the
                 * range 128 -> 155 (inclusive) for self-configurable ECUs.
                 * 
                 * In order to play it safe, we'll allow source addresses 
                 * greater than or equal to 128
                 */
                if (eld_src_add >= 128)
                {
                    return true;
                }
                else
                {
                    Serial.print(eld_src_add, HEX);
                    Serial.println(": address in privileged range");
                    return false;
                }
            }

            /* Cycle through the allowed-byte-2 array and if byte 2 value 
             * matches any element in that array, return True.
             */
            for (int j = 0; j < ab2s_len; j++)
            {
                if (allwd_byte2s[j] == b2)
                {
                    return true;
                }
            }
        }
    }
    else
    {
        /* Get values in bytes 2 and 3 */
        val = msg_ptr->id;
        val = val << 8;
        val = val >> 16;
        pgn = val;

        for (int j = 0; j < ab23s_len; j++)
        {
            if (allwd_byte23s[j] == pgn)
            {
                return true;
            }
        }
    }
    
    return false;
}


bool
isAllowedReqPGN(const uint8_t * msgbuf_ptr)
{
    uint32_t req_pgn;
    int      arp_len;

    /* PGN in CAN Id is a Request PGN. The PGN value in msgbuf_ptr
     * should be one of:
     *  54016   Calibration Information (DM19)
     *  60928   Address Claim (Omnitracs asking ECUs to report claimed address)
     *  65216   Service Information
     *  65242   Software Identification
     *  65244   Idle Operation
     *  65253   Engine Hours, Revolutions
     *  65257   Fuel Consumption (Liquid) 1
     *  65259   Component Identification
     *  65260   Vehicle Identification
     */
    req_pgn = msgbuf_ptr[2];
    req_pgn = req_pgn << 8;
    req_pgn = req_pgn | msgbuf_ptr[1];
    req_pgn = req_pgn << 8;
    req_pgn = req_pgn | msgbuf_ptr[0];

    Serial.print("req_pgn: ");
    Serial.print(req_pgn);
    Serial.print(" ");
    Serial.println(req_pgn, HEX);

    arp_len = sizeof(allwd_req_pgns) / sizeof(allwd_req_pgns[0]);

    for (int i = 0; i < arp_len; i++)
    {
        if (allwd_req_pgns[i] == req_pgn)
            return true;
    }

    return false;
}
