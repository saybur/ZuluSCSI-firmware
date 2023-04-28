/*  
 *  ZuluSCSI
 *  Copyright (c) 2022 Rabbit Hole Computing
 * 
 * This project is based on BlueSCSI:
 *
 *  BlueSCSI
 *  Copyright (c) 2021  Eric Helgeson, Androda
 *  
 *  This file is free software: you may copy, redistribute and/or modify it  
 *  under the terms of the GNU General Public License as published by the  
 *  Free Software Foundation, either version 2 of the License, or (at your  
 *  option) any later version.  
 *  
 *  This file is distributed in the hope that it will be useful, but  
 *  WITHOUT ANY WARRANTY; without even the implied warranty of  
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU  
 *  General Public License for more details.  
 *  
 *  You should have received a copy of the GNU General Public License  
 *  along with this program.  If not, see https://github.com/erichelgeson/bluescsi.  
 *  
 * This file incorporates work covered by the following copyright and  
 * permission notice:  
 *  
 *     Copyright (c) 2019 komatsu   
 *  
 *     Permission to use, copy, modify, and/or distribute this software  
 *     for any purpose with or without fee is hereby granted, provided  
 *     that the above copyright notice and this permission notice appear  
 *     in all copies.  
 *  
 *     THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL  
 *     WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED  
 *     WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE  
 *     AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR  
 *     CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS  
 *     OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT,  
 *     NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN  
 *     CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.  
 */

#include <SdFat.h>
#include <minIni.h>
#include <minIni_cache.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include "ZuluSCSI_config.h"
#include "ZuluSCSI_platform.h"
#include "ZuluSCSI_log.h"
#include "ZuluSCSI_log_trace.h"
#include "ZuluSCSI_disk.h"
#include "ZuluSCSI_initiator.h"

SdFs SD;
FsFile g_logfile;
static bool g_romdrive_active;
static bool g_sdcard_present;

/************************************/
/* Status reporting by blinking led */
/************************************/

#define BLINK_STATUS_OK 1
#define BLINK_ERROR_NO_IMAGES  3 
#define BLINK_ERROR_NO_SD_CARD 5

void blinkStatus(int count)
{
  for (int i = 0; i < count; i++)
  {
    LED_ON();
    delay(250);
    LED_OFF();
    delay(250);
  }
}

extern "C" void s2s_ledOn()
{
  LED_ON();
}

extern "C" void s2s_ledOff()
{
  LED_OFF();
}

/**************/
/* Log saving */
/**************/

void save_logfile(bool always = false)
{
  static uint32_t prev_log_pos = 0;
  static uint32_t prev_log_len = 0;
  static uint32_t prev_log_save = 0;
  uint32_t loglen = log_get_buffer_len();

  if (loglen != prev_log_len && g_sdcard_present)
  {
    // When debug is off, save log at most every LOG_SAVE_INTERVAL_MS
    // When debug is on, save after every SCSI command.
    if (always || g_log_debug || (LOG_SAVE_INTERVAL_MS > 0 && (uint32_t)(millis() - prev_log_save) > LOG_SAVE_INTERVAL_MS))
    {
      g_logfile.write(log_get_buffer(&prev_log_pos));
      g_logfile.flush();
      
      prev_log_len = loglen;
      prev_log_save = millis();
    }
  }
}

void init_logfile()
{
  static bool first_open_after_boot = true;

  bool truncate = first_open_after_boot;
  int flags = O_WRONLY | O_CREAT | (truncate ? O_TRUNC : O_APPEND);
  g_logfile = SD.open(LOGFILE, flags);
  if (!g_logfile.isOpen())
  {
    logmsg("Failed to open log file: ", SD.sdErrorCode());
  }
  save_logfile(true);

  first_open_after_boot = false;
}

void print_sd_info()
{
  uint64_t size = (uint64_t)SD.vol()->clusterCount() * SD.vol()->bytesPerCluster();
  logmsg("SD card detected, FAT", (int)SD.vol()->fatType(),
          " volume size: ", (int)(size / 1024 / 1024), " MB");
  
  cid_t sd_cid;

  if(SD.card()->readCID(&sd_cid))
  {
    logmsg("SD MID: ", (uint8_t)sd_cid.mid, ", OID: ", (uint8_t)sd_cid.oid[0], " ", (uint8_t)sd_cid.oid[1]);
    
    char sdname[6] = {sd_cid.pnm[0], sd_cid.pnm[1], sd_cid.pnm[2], sd_cid.pnm[3], sd_cid.pnm[4], 0};
    logmsg("SD Name: ", sdname);
    logmsg("SD Date: ", (int)sd_cid.mdtMonth(), "/", sd_cid.mdtYear());
    logmsg("SD Serial: ", sd_cid.psn());
  }
}

/*********************************/
/* Harddisk image file handling  */
/*********************************/

// When a file is called e.g. "Create_1024M_HD40.txt",
// create image file with specified size.
// Returns true if image file creation succeeded.
//
// Parsing rules:
// - Filename must start with "Create", case-insensitive
// - Separator can be either underscore, dash or space
// - Size must start with a number. Unit of k, kb, m, mb, g, gb is supported,
//   case-insensitive, with 1024 as the base. If no unit, assume MB.
// - If target filename does not have extension (just .txt), use ".bin"
bool createImage(const char *cmd_filename, char imgname[MAX_FILE_PATH + 1])
{
  if (strncasecmp(cmd_filename, CREATEFILE, strlen(CREATEFILE)) != 0)
  {
    return false;
  }

  const char *p = cmd_filename + strlen(CREATEFILE);

  // Skip separator if any
  while (isspace(*p) || *p == '-' || *p == '_')
  {
    p++;
  }

  char *unit = nullptr;
  uint64_t size = strtoul(p, &unit, 10);

  if (size <= 0 || unit <= p)
  {
    logmsg("---- Could not parse size in filename '", cmd_filename, "'");
    return false;
  }

  // Parse k/M/G unit
  char unitchar = tolower(*unit);
  if (unitchar == 'k')
  {
    size *= 1024;
    p = unit + 1;
  }
  else if (unitchar == 'm')
  {
    size *= 1024 * 1024;
    p = unit + 1;
  }
  else if (unitchar == 'g')
  {
    size *= 1024 * 1024 * 1024;
    p = unit + 1;
  }
  else
  {
    size *= 1024 * 1024;
    p = unit;
  }

  // Skip i and B if part of unit
  if (tolower(*p) == 'i') p++;
  if (tolower(*p) == 'b') p++;

  // Skip separator if any
  while (isspace(*p) || *p == '-' || *p == '_')
  {
    p++;
  }

  // Copy target filename to new buffer
  strncpy(imgname, p, MAX_FILE_PATH);
  imgname[MAX_FILE_PATH] = '\0';
  int namelen = strlen(imgname);

  // Strip .txt extension if any
  if (namelen >= 4 && strncasecmp(imgname + namelen - 4, ".txt", 4) == 0)
  {
    namelen -= 4;
    imgname[namelen] = '\0';
  }

  // Add .bin if no extension
  if (!strchr(imgname, '.') && namelen < MAX_FILE_PATH - 4)
  {
    namelen += 4;
    strcat(imgname, ".bin");
  }

  // Check if file exists
  if (namelen <= 5 || SD.exists(imgname))
  {
    logmsg("---- Image file already exists, skipping '", cmd_filename, "'");
    return false;
  }

  // Create file, try to preallocate contiguous sectors
  LED_ON();
  FsFile file = SD.open(imgname, O_WRONLY | O_CREAT);

  if (!file.preAllocate(size))
  {
    logmsg("---- Preallocation didn't find contiguous set of clusters, continuing anyway");
  }

  // Write zeros to fill the file
  uint32_t start = millis();
  memset(scsiDev.data, 0, sizeof(scsiDev.data));
  uint64_t remain = size;
  while (remain > 0)
  {
    if (millis() & 128) { LED_ON(); } else { LED_OFF(); }
    platform_reset_watchdog();

    size_t to_write = sizeof(scsiDev.data);
    if (to_write > remain) to_write = remain;
    if (file.write(scsiDev.data, to_write) != to_write)
    {
      logmsg("---- File writing to '", imgname, "' failed with ", (int)remain, " bytes remaining");
      file.close();
      LED_OFF();
      return false;
    }

    remain -= to_write;
  }

  file.close();
  uint32_t time = millis() - start;
  int kb_per_s = size / time;
  logmsg("---- Image creation successful, write speed ", kb_per_s, " kB/s, removing '", cmd_filename, "'");
  SD.remove(cmd_filename);

  LED_OFF();
  return true;
}

// Iterate over the root path in the SD card looking for candidate image files.
bool findHDDImages()
{
  char imgdir[MAX_FILE_PATH];
  ini_gets("SCSI", "Dir", "/", imgdir, sizeof(imgdir), CONFIGFILE);
  int dirindex = 0;

  logmsg("Finding HDD images in directory ", imgdir, ":");

  SdFile root;
  root.open(imgdir);
  if (!root.isOpen())
  {
    logmsg("Could not open directory: ", imgdir);
  }

  SdFile file;
  bool imageReady;
  bool foundImage = false;
  int usedDefaultId = 0;
  while (1)
  {
    if (!file.openNext(&root, O_READ))
    {
      // Check for additional directories with ini keys Dir1..Dir9
      while (dirindex < 10)
      {
        dirindex++;
        char key[5] = "Dir0";
        key[3] += dirindex;
        if (ini_gets("SCSI", key, "", imgdir, sizeof(imgdir), CONFIGFILE) != 0)
        {
          break;
        }
      }

      if (imgdir[0] != '\0')
      {
        logmsg("Finding HDD images in additional directory Dir", (int)dirindex, " = \"", imgdir, "\":");
        root.open(imgdir);
        if (!root.isOpen())
        {
          logmsg("-- Could not open directory: ", imgdir);
        }
        continue;
      }
      else
      {
        break;
      }
    }

    char name[MAX_FILE_PATH+1];
    if(!file.isDir()) {
      file.getName(name, MAX_FILE_PATH+1);
      file.close();

      // Special filename for clearing any previously programmed ROM drive
      if(strcasecmp(name, "CLEAR_ROM") == 0)
      {
        logmsg("-- Special filename: '", name, "'");
        scsiDiskClearRomDrive();
        continue;
      }

      // Special filename for creating new empty image files
      if (strncasecmp(name, CREATEFILE, strlen(CREATEFILE)) == 0)
      {
        logmsg("-- Special filename: '", name, "'");
        char imgname[MAX_FILE_PATH+1];
        if (createImage(name, imgname))
        {
          // Created new image file, use its name instead of the name of the command file
          strncpy(name, imgname, MAX_FILE_PATH);
          name[MAX_FILE_PATH] = '\0';
        }
      }

      bool is_hd = (tolower(name[0]) == 'h' && tolower(name[1]) == 'd');
      bool is_cd = (tolower(name[0]) == 'c' && tolower(name[1]) == 'd');
      bool is_fd = (tolower(name[0]) == 'f' && tolower(name[1]) == 'd');
      bool is_mo = (tolower(name[0]) == 'm' && tolower(name[1]) == 'o');
      bool is_re = (tolower(name[0]) == 'r' && tolower(name[1]) == 'e');
      bool is_tp = (tolower(name[0]) == 't' && tolower(name[1]) == 'p');
      if (is_hd || is_cd || is_fd || is_mo || is_re || is_tp)
      {
        // Check file extension
        // We accept anything except known compressed files
        bool is_compressed = false;
        const char *extension = strrchr(name, '.');
        if (extension)
        {
          const char *archive_exts[] = {
            ".tar", ".tgz", ".gz", ".bz2", ".tbz2", ".xz", ".zst", ".z",
            ".zip", ".zipx", ".rar", ".lzh", ".lha", ".lzo", ".lz4", ".arj",
            ".dmg", ".hqx", ".cpt", ".7z", ".s7z",
            NULL
          };

          for (int i = 0; archive_exts[i]; i++)
          {
            if (strcasecmp(extension, archive_exts[i]) == 0)
            {
              is_compressed = true;
              break;
            }
          }
        }

        if (is_compressed)
        {
          logmsg("-- Ignoring compressed file ", name);
          continue;
        }

        // Check if the image should be loaded to microcontroller flash ROM drive
        bool is_romdrive = false;
        if (extension && strcasecmp(extension, ".rom") == 0)
        {
          is_romdrive = true;
        }
        else if (extension && strcasecmp(extension, ".rom_loaded") == 0)
        {
          // Already loaded ROM drive, ignore the image
          continue;
        }

        // Defaults for Hard Disks
        int id  = 1; // 0 and 3 are common in Macs for physical HD and CD, so avoid them.
        int lun = 0;
        int blk = 512;

        if (is_cd)
        {
          // Use 2048 as the default sector size for CD-ROMs
          blk = 2048;
        }

        // Parse SCSI device ID
        int file_name_length = strlen(name);
        if(file_name_length > 2) { // HD[N]
          int tmp_id = name[HDIMG_ID_POS] - '0';

          if(tmp_id > -1 && tmp_id < 8)
          {
            id = tmp_id; // If valid id, set it, else use default
          }
          else
          {
            id = usedDefaultId++;
          }
        }

        // Parse SCSI LUN number
        if(file_name_length > 3) { // HD0[N]
          int tmp_lun = name[HDIMG_LUN_POS] - '0';

          if(tmp_lun > -1 && tmp_lun < NUM_SCSILUN) {
            lun = tmp_lun; // If valid id, set it, else use default
          }
        }

        // Parse block size (HD00_NNNN)
        const char *blksize = strchr(name, '_');
        if (blksize)
        {
          int blktmp = strtoul(blksize + 1, NULL, 10);
          if (blktmp == 256 || blktmp == 512 || blktmp == 1024 ||
              blktmp == 2048 || blktmp == 4096 || blktmp == 8192)
          {
            blk = blktmp;
          }
        }

        // Add the directory name to get the full file path
        char fullname[MAX_FILE_PATH * 2 + 2] = {0};
        strncpy(fullname, imgdir, MAX_FILE_PATH);
        if (fullname[strlen(fullname) - 1] != '/') strcat(fullname, "/");
        strcat(fullname, name);

        // Check whether this SCSI ID has been configured yet
        if (s2s_getConfigById(id))
        {
          logmsg("-- Ignoring ", fullname, ", SCSI ID ", id, " is already in use!");
          continue;
        }

        // Type mapping based on filename.
        // If type is FIXED, the type can still be overridden in .ini file.
        S2S_CFG_TYPE type = S2S_CFG_FIXED;
        if (is_cd) type = S2S_CFG_OPTICAL;
        if (is_fd) type = S2S_CFG_FLOPPY_14MB;
        if (is_mo) type = S2S_CFG_MO;
        if (is_re) type = S2S_CFG_REMOVEABLE;
        if (is_tp) type = S2S_CFG_SEQUENTIAL;

        // Open the image file
        if (id < NUM_SCSIID && is_romdrive)
        {
          logmsg("-- Loading ROM drive from ", fullname, " for id:", id);
          imageReady = scsiDiskProgramRomDrive(fullname, id, blk, type);
          
          if (imageReady)
          {
            foundImage = true;
          }
        }
        else if(id < NUM_SCSIID && lun < NUM_SCSILUN) {
          logmsg("-- Opening ", fullname, " for id:", id, " lun:", lun);

          imageReady = scsiDiskOpenHDDImage(id, fullname, id, lun, blk, type);
          if(imageReady)
          {
            foundImage = true;
          }
          else
          {
            logmsg("---- Failed to load image");
          }
        } else {
          logmsg("-- Invalid lun or id for image ", fullname);
        }
      }
    }
  }

  if(usedDefaultId > 0) {
    logmsg("Some images did not specify a SCSI ID. Last file will be used at ID ", usedDefaultId);
  }
  root.close();

  g_romdrive_active = scsiDiskActivateRomDrive();

  // Print SCSI drive map
  for (int i = 0; i < NUM_SCSIID; i++)
  {
    const S2S_TargetCfg* cfg = s2s_getConfigByIndex(i);
    
    if (cfg && (cfg->scsiId & S2S_CFG_TARGET_ENABLED))
    {
      int capacity_kB = ((uint64_t)cfg->scsiSectors * cfg->bytesPerSector) / 1024;
      logmsg("SCSI ID:", (int)(cfg->scsiId & 7),
            " BlockSize:", (int)cfg->bytesPerSector,
            " Type:", (int)cfg->deviceType,
            " Quirks:", (int)cfg->quirks,
            " ImageSize:", capacity_kB, "kB");
    }
  }

  return foundImage;
}

/************************/
/* Config file loading  */
/************************/

void readSCSIDeviceConfig()
{
  s2s_configInit(&scsiDev.boardCfg);

  for (int i = 0; i < NUM_SCSIID; i++)
  {
    scsiDiskLoadConfig(i);
  }
}

/*********************************/
/* Main SCSI handling loop       */
/*********************************/

static bool mountSDCard()
{
  invalidate_ini_cache();

  // Check for the common case, FAT filesystem as first partition
  if (SD.begin(SD_CONFIG))
  {
    reload_ini_cache(CONFIGFILE);
    return true;
  }

  // Do we have any kind of card?
  if (!SD.card() || SD.sdErrorCode() != 0)
    return false;

  // Try to mount the whole card as FAT (without partition table)
  if (static_cast<FsVolume*>(&SD)->begin(SD.card(), true, 0))
    return true;

  // Failed to mount FAT filesystem, but card can still be accessed as raw image
  return true;
}

static void reinitSCSI()
{
  if (ini_getbool("SCSI", "Debug", 0, CONFIGFILE))
  {
    g_log_debug = true;
  }

#ifdef PLATFORM_HAS_INITIATOR_MODE
  if (platform_is_initiator_mode_enabled())
  {
    // Initialize scsiDev to zero values even though it is not used
    scsiInit();

    // Initializer initiator mode state machine
    scsiInitiatorInit();

    blinkStatus(BLINK_STATUS_OK);

    return;
  }
#endif

  scsiDiskResetImages();
  readSCSIDeviceConfig();
  findHDDImages();

  // Error if there are 0 image files
  if (scsiDiskCheckAnyImagesConfigured())
  {
    // Ok, there is an image, turn LED on for the time it takes to perform init
    LED_ON();
    delay(100);
  }
  else
  {
#if RAW_FALLBACK_ENABLE
    logmsg("No images found, enabling RAW fallback partition");
    scsiDiskOpenHDDImage(RAW_FALLBACK_SCSI_ID, "RAW:0:0xFFFFFFFF", RAW_FALLBACK_SCSI_ID, 0,
                         RAW_FALLBACK_BLOCKSIZE);
#else
    logmsg("No valid image files found!");
#endif
    blinkStatus(BLINK_ERROR_NO_IMAGES);
  }

  scsiPhyReset();
  scsiDiskInit();
  scsiInit();
  
}

extern "C" void zuluscsi_setup(void)
{
  platform_init();
  platform_late_init();

  g_sdcard_present = mountSDCard();

  if(!g_sdcard_present)
  {
    logmsg("SD card init failed, sdErrorCode: ", (int)SD.sdErrorCode(),
           " sdErrorData: ", (int)SD.sdErrorData());

    if (scsiDiskCheckRomDrive())
    {
      reinitSCSI();
      if (g_romdrive_active)
      {
        logmsg("Enabled ROM drive without SD card");
        return;
      }
    }

    do
    {
      blinkStatus(BLINK_ERROR_NO_SD_CARD);
      delay(1000);
      platform_reset_watchdog();
      g_sdcard_present = mountSDCard();
    } while (!g_sdcard_present);
    logmsg("SD card init succeeded after retry");
  }

  if (g_sdcard_present)
  {
    if (SD.clusterCount() == 0)
    {
      logmsg("SD card without filesystem!");
    }

    print_sd_info();
  
    reinitSCSI();
  }

  logmsg("Initialization complete!");

  if (g_sdcard_present)
  {
    init_logfile();
    if (ini_getbool("SCSI", "DisableStatusLED", false, CONFIGFILE))
    {
      platform_disable_led();
    }
  }

  // Counterpart for the LED_ON in reinitSCSI().
  LED_OFF();
}

extern "C" void zuluscsi_main_loop(void)
{
  static uint32_t sd_card_check_time = 0;
  static uint32_t last_request_time = 0;

  platform_reset_watchdog();
  
#ifdef PLATFORM_HAS_INITIATOR_MODE
  if (platform_is_initiator_mode_enabled())
  {
    scsiInitiatorMainLoop();
    save_logfile();
  }
  else
#endif
  {
    scsiPoll();
    scsiDiskPoll();
    scsiLogPhaseChange(scsiDev.phase);

    // Save log periodically during status phase if there are new messages.
    // In debug mode, also save every 2 seconds if no SCSI requests come in.
    // SD card writing takes a while, during which the code can't handle new
    // SCSI requests, so normally we only want to save during a phase where
    // the host is waiting for us. But for debugging issues where no requests
    // come through or a request hangs, it's useful to force saving of log.
    if (scsiDev.phase == STATUS || (g_log_debug && (uint32_t)(millis() - last_request_time) > 2000))
    {
      save_logfile();
      last_request_time = millis();
    }
  }

  if (g_sdcard_present)
  {
    // Check SD card status for hotplug
    if (scsiDev.phase == BUS_FREE &&
        (uint32_t)(millis() - sd_card_check_time) > 5000)
    {
      sd_card_check_time = millis();
      uint32_t ocr;
      if (!SD.card()->readOCR(&ocr))
      {
        if (!SD.card()->readOCR(&ocr))
        {
          g_sdcard_present = false;
          logmsg("SD card removed, trying to reinit");
        }
      }
    }
  }

  if (!g_sdcard_present)
  {
    // Try to remount SD card
    do 
    {
      g_sdcard_present = mountSDCard();

      if (g_sdcard_present)
      {
        logmsg("SD card reinit succeeded");
        print_sd_info();

        reinitSCSI();
        init_logfile();
      }
      else if (!g_romdrive_active)
      {
        blinkStatus(BLINK_ERROR_NO_SD_CARD);
        delay(1000);
        platform_reset_watchdog();
      }
    } while (!g_sdcard_present && !g_romdrive_active);
  }
  else
  {
    
  }
}
