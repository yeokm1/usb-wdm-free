/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Taken from libusb-win32 0.1.12.2 (git 00eb396), this path.
 */
/* LIBUSB-WIN32, Generic Windows USB Library
 * Copyright (c) 2002-2005 Stephan Meyer <ste_meyer@web.de>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */


#include <windows.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __GNUC__
#include <ddk/cfgmgr32.h>
#else
#include <cfgmgr32.h>
#define strlwr(p) _strlwr(p)
#endif

#include "registry.h"
#include "error.h"

/* usb-wdm-free: the class-filter code (class keys, UpperFilters/
 * LowerFilters writers, the multi-string editors and the restart of every
 * root hub) is removed with filter mode (design 2.7 item 3, 6.6 commit 3). */

static bool_t usb_registry_set_device_state(DWORD state, HDEVINFO dev_info,
                                            SP_DEVINFO_DATA *dev_info_data);


bool_t usb_registry_is_nt(void)
{
  return GetVersion() < 0x80000000 ? TRUE : FALSE;
}

bool_t usb_registry_get_property(DWORD which, HDEVINFO dev_info, 
                                 SP_DEVINFO_DATA *dev_info_data,
                                 char *buf, int size)
{
  DWORD reg_type;
  DWORD key_type;
  DWORD length = size;
  char *p = NULL;
  char *val_name = NULL;
  HKEY reg_key = NULL;
  
  memset(buf, 0, size);

  switch(which)
    {
    case SPDRP_LOWERFILTERS:
      val_name = "LowerFilters";
      key_type = DIREG_DEV; 
      break;
    case SPDRP_UPPERFILTERS:
      val_name = "UpperFilters";
      key_type = DIREG_DEV; 
      break;
    case SPDRP_SERVICE:
      val_name = "NTMPDriver";
      key_type = DIREG_DRV;
      break;
    case SPDRP_CLASSGUID:
      val_name = "ClassGUID";
      key_type = DIREG_DEV;    
    case SPDRP_CLASS:
      val_name = "Class";
      key_type = DIREG_DEV;    
      break;
    case SPDRP_HARDWAREID:
      val_name = "HardwareID";
      key_type = DIREG_DEV;
      break;
    case SPDRP_DEVICEDESC:
      val_name = "DeviceDesc";
      key_type = DIREG_DEV;
      break;
    case SPDRP_MFG:
      val_name = "Mfg";
      key_type = DIREG_DEV;
      break;

    default:
      return FALSE;
    }

  if(usb_registry_is_nt())
    {
      if(!SetupDiGetDeviceRegistryProperty(dev_info, dev_info_data, which,  
                                           &reg_type, (BYTE *)buf, 
                                           size, &length))
        {
          return FALSE;
        }

      if(length <= 2)
        {
          return FALSE;
        }
    }
  else /* Win9x */
    {
      reg_key = SetupDiOpenDevRegKey(dev_info, dev_info_data, 
                                     DICS_FLAG_GLOBAL, 
                                     0, key_type, KEY_ALL_ACCESS);
      
      if(reg_key == INVALID_HANDLE_VALUE)
        {
          usb_error("usb_registry_get_property(): reading "
                    "registry key failed");
          return FALSE;
        }
      
      if(RegQueryValueEx(reg_key, val_name, NULL, &reg_type, 
                         (BYTE *)buf, &length) != ERROR_SUCCESS
         || length <= 2)
        {
          RegCloseKey(reg_key);
          return FALSE;
        }

      RegCloseKey(reg_key);
      
      if(reg_type == REG_MULTI_SZ)
        {
          p = buf;
          while(*p)
            {
              if(*p == ',')
                {
                  *p = 0;
                }
              p++;
            }
        }
    }
 
  return TRUE;
}

static bool_t usb_registry_set_device_state(DWORD state, HDEVINFO dev_info, 
                                            SP_DEVINFO_DATA *dev_info_data)
{
  SP_PROPCHANGE_PARAMS prop_params;

  memset(&prop_params, 0, sizeof(SP_PROPCHANGE_PARAMS));

  prop_params.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
  prop_params.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
  prop_params.StateChange = state;
  prop_params.Scope = DICS_FLAG_CONFIGSPECIFIC;//DICS_FLAG_GLOBAL;
  prop_params.HwProfile = 0;
	  

  if(!SetupDiSetClassInstallParams(dev_info, dev_info_data,
                                   (SP_CLASSINSTALL_HEADER *)&prop_params,
                                   sizeof(SP_PROPCHANGE_PARAMS)))
    {
      usb_error("usb_registry_set_device_state(): setting class "
                "install parameters failed");
      return FALSE;
    }
	  

  if(!SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, dev_info, dev_info_data))
    {
      usb_error("usb_registry_set_device_state(): calling class "
                "installer failed");
      return FALSE;
    }

  return TRUE;
}

bool_t usb_registry_restart_device(HDEVINFO dev_info, 
                                   SP_DEVINFO_DATA *dev_info_data)
{
  return usb_registry_set_device_state(DICS_PROPCHANGE, dev_info, 
                                       dev_info_data);
}

bool_t usb_registry_stop_device(HDEVINFO dev_info, 
                                SP_DEVINFO_DATA *dev_info_data)
{
  return usb_registry_set_device_state(DICS_DISABLE, dev_info, 
                                       dev_info_data);
}

bool_t usb_registry_start_device(HDEVINFO dev_info, 
                                 SP_DEVINFO_DATA *dev_info_data)
{
  return usb_registry_set_device_state(DICS_ENABLE, dev_info, 
                                       dev_info_data);
}

bool_t usb_registry_is_service_libusb(HDEVINFO dev_info, 
                                      SP_DEVINFO_DATA *dev_info_data)
{
  char service_name[MAX_PATH];

  if(!usb_registry_get_property(SPDRP_SERVICE, dev_info, dev_info_data,
                                service_name, sizeof(service_name)))
    {
      return FALSE;
    }
  
  usb_registry_mz_string_lower(service_name);
   
  if(usb_registry_mz_string_find_sub(service_name, "libusb"))
    {
      return TRUE;
    }

  return FALSE;
}

void usb_registry_stop_libusb_devices(void)
{
  HDEVINFO dev_info;
  SP_DEVINFO_DATA dev_info_data;
  int dev_index = 0;
  
  dev_info_data.cbSize = sizeof(SP_DEVINFO_DATA);
  dev_index = 0;

  dev_info = SetupDiGetClassDevs(NULL, "USB", NULL,
                                 DIGCF_ALLCLASSES | DIGCF_PRESENT);
  
  if(dev_info == INVALID_HANDLE_VALUE)
    {
      usb_error("usb_registry_stop_libusb_devices(): getting "
                "device info set failed");
      return;
    }
  
  while(SetupDiEnumDeviceInfo(dev_info, dev_index, &dev_info_data))
    {
      if(usb_registry_is_service_libusb(dev_info, &dev_info_data))
        {
          usb_registry_stop_device(dev_info, &dev_info_data);
        }
      dev_index++;
    }
  
  SetupDiDestroyDeviceInfoList(dev_info);
}

void usb_registry_start_libusb_devices(void)
{
  HDEVINFO dev_info;
  SP_DEVINFO_DATA dev_info_data;
  int dev_index = 0;
  
  dev_info_data.cbSize = sizeof(SP_DEVINFO_DATA);
  dev_index = 0;

  dev_info = SetupDiGetClassDevs(NULL, "USB", NULL,
                                 DIGCF_ALLCLASSES | DIGCF_PRESENT);
  
  if(dev_info == INVALID_HANDLE_VALUE)
    {
      usb_error("usb_registry_stop_libusb_devices(): getting "
                "device info set failed");
      return;
    }
  
  while(SetupDiEnumDeviceInfo(dev_info, dev_index, &dev_info_data))
    {
      if(usb_registry_is_service_libusb(dev_info, &dev_info_data))
        {
          usb_registry_start_device(dev_info, &dev_info_data);
        }
      dev_index++;
    }
  
  SetupDiDestroyDeviceInfoList(dev_info);
}


bool_t usb_registry_match(HDEVINFO dev_info, 
                          SP_DEVINFO_DATA *dev_info_data)
{
  char tmp[MAX_PATH];
  
  if(!usb_registry_get_property(SPDRP_HARDWAREID, dev_info, dev_info_data,
                                tmp, sizeof(tmp)))
    {
      usb_error("usb_registry_match_no_hubs(): getting hardware id "
                "failed");
      return FALSE;
    }

  usb_registry_mz_string_lower(tmp);

  /* search for USB devices, skip root hubs and interfaces of composite */
  /* devices */
  if(usb_registry_mz_string_find_sub(tmp, "&mi_")
     || usb_registry_mz_string_find_sub(tmp, "root_hub"))
    {
      return FALSE;
    }

  return TRUE;
}

int usb_registry_mz_string_size(const char *src)
{
  char *p = (char *)src;
  
  if(!src)
    {
      return 0;
    }

  while(*p)
    {
      p += (strlen(p) + 1);
    }
  
  return (int)(p - src) + 1;
}

char *usb_registry_mz_string_find_sub(const char *src, const char *str)
{
  while(*src)
    {
      if(strstr(src, str))
        {
          return (char *)src;
        }
      src += (strlen(src) + 1);
    }

  return NULL;
}

char *usb_registry_mz_string_find(const char *src, const char *str)
{
  while(*src)
    {
      if(!strcmp(src, str))
        {
          return (char *)src;
        }
      src += strlen(src) + 1;
    }

  return NULL;
}

void usb_registry_mz_string_lower(char *src)
{
  while(*src)
    {
      strlwr(src);
      src += (strlen(src) + 1);
    }
}
