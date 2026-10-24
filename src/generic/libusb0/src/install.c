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
#include <winsvc.h>
#include <setupapi.h>
#include <stdio.h>
#include <regstr.h>
#include <wchar.h>
#include <string.h>

#ifdef __GNUC__
#include <ddk/cfgmgr32.h>
#else
#include <cfgmgr32.h>
#define strlwr(p) _strlwr(p)
#endif

#include "usb.h"
#include "registry.h"
#include "error.h"
#include "driver_api.h"



#define INSTALLFLAG_FORCE 0x00000001

/* newdev.dll exports */
typedef BOOL (WINAPI * update_driver_for_plug_and_play_devices_t)(HWND, 
                                                                  LPCSTR, 
                                                                  LPCSTR, 
                                                                  DWORD,
                                                                  PBOOL);
/* setupapi.dll exports */
typedef BOOL (WINAPI * setup_copy_oem_inf_t)(PCSTR, PCSTR, DWORD, DWORD,
                                             PSTR, DWORD, PDWORD, PSTR*);

void CALLBACK usb_touch_inf_file_rundll(HWND wnd, HINSTANCE instance,
                                        LPSTR cmd_line, int cmd_show);

void CALLBACK usb_install_service_np_rundll(HWND wnd, HINSTANCE instance,
                                            LPSTR cmd_line, int cmd_show)
{
  usb_install_service_np();
}

void CALLBACK usb_uninstall_service_np_rundll(HWND wnd, HINSTANCE instance,
                                              LPSTR cmd_line, int cmd_show)
{
  usb_uninstall_service_np();
}

void CALLBACK usb_install_driver_np_rundll(HWND wnd, HINSTANCE instance,
                                           LPSTR cmd_line, int cmd_show)
{
  usb_install_driver_np(cmd_line);
}


/* usb-wdm-free: filter mode is removed (docs/design/libusb.md 2.7 item 3,
 * 6.6 commit 3). Upstream's two functions installed and removed
 * libusb0.sys as an upper filter of every USB device class, then restarted
 * every root hub; they now change nothing and report an error. A device is
 * bound by LIBUSB0.INF. */
int usb_install_service_np(void)
{
  usb_error("usb_install_service_np(): not supported: filter mode is "
            "removed; install a device with LIBUSB0.INF");
  return -1;
}

int usb_uninstall_service_np(void)
{
  usb_error("usb_uninstall_service_np(): not supported: filter mode is "
            "removed");
  return -1;
}

int usb_install_driver_np(const char *inf_file)
{
  HDEVINFO dev_info;
  SP_DEVINFO_DATA dev_info_data;
  INFCONTEXT inf_context;
  HINF inf_handle;
  DWORD config_flags, problem, status;
  BOOL reboot;
  char inf_path[MAX_PATH];
  char id[MAX_PATH];
  char tmp_id[MAX_PATH];
  char *p;
  int dev_index;
  HINSTANCE newdev_dll = NULL;
  HMODULE setupapi_dll = NULL;

  update_driver_for_plug_and_play_devices_t UpdateDriverForPlugAndPlayDevices;
  setup_copy_oem_inf_t SetupCopyOEMInf;
  newdev_dll = LoadLibrary("newdev.dll");

  if(!newdev_dll)
    {
      usb_error("usb_install_driver(): loading newdev.dll failed\n");
      return -1;
    }
  
  UpdateDriverForPlugAndPlayDevices =  
    (update_driver_for_plug_and_play_devices_t) 
    GetProcAddress(newdev_dll, "UpdateDriverForPlugAndPlayDevicesA");

  if(!UpdateDriverForPlugAndPlayDevices)
    {
      usb_error("usb_install_driver(): loading newdev.dll failed\n");
      return -1;
    }

  setupapi_dll = GetModuleHandle("setupapi.dll");
  
  if(!setupapi_dll)
    {
      usb_error("usb_install_driver(): loading setupapi.dll failed\n");
      return -1;
    }
  SetupCopyOEMInf = (setup_copy_oem_inf_t)
    GetProcAddress(setupapi_dll, "SetupCopyOEMInfA");
  
  if(!SetupCopyOEMInf)
    {
      usb_error("usb_install_driver(): loading setupapi.dll failed\n");
      return -1;
    }

  dev_info_data.cbSize = sizeof(SP_DEVINFO_DATA);


  /* retrieve the full .inf file path */
  if(!GetFullPathName(inf_file, MAX_PATH, inf_path, NULL))
    {
      usb_error("usb_install_driver(): .inf file %s not found\n", 
                inf_file);
      return -1;
    }

  /* open the .inf file */
  inf_handle = SetupOpenInfFile(inf_path, NULL, INF_STYLE_WIN4, NULL);

  if(inf_handle == INVALID_HANDLE_VALUE)
    {
      usb_error("usb_install_driver(): unable to open .inf file %s\n", 
                inf_file);
      return -1;
    }

  /* find the .inf file's device description section marked "Devices" */
  if(!SetupFindFirstLine(inf_handle, "Devices", NULL, &inf_context))
    {
      usb_error("usb_install_driver(): .inf file %s does not contain "
                "any device descriptions\n", inf_file);
      SetupCloseInfFile(inf_handle);
      return -1;
    }


  do {
    /* get the device ID from the .inf file */
    if(!SetupGetStringField(&inf_context, 2, id, sizeof(id), NULL))
      {
        continue;
      }

    /* convert the string to lowercase */
    strlwr(id);

    reboot = FALSE;

    /* copy the .inf file to the system directory so that is will be found */
    /* when new devices are plugged in */
    SetupCopyOEMInf(inf_path, NULL, SPOST_PATH, 0, NULL, 0, NULL, NULL);

    /* update all connected devices matching this ID, but only if this */
    /* driver is better or newer */
    UpdateDriverForPlugAndPlayDevices(NULL, id, inf_path, INSTALLFLAG_FORCE, 
                                      &reboot);
    

    /* now search the registry for device nodes representing currently  */
    /* unattached devices */


    /* get all USB device nodes from the registry, present and non-present */
    /* devices */
    dev_info = SetupDiGetClassDevs(NULL, "USB", NULL, DIGCF_ALLCLASSES);
    
    if(dev_info == INVALID_HANDLE_VALUE)
      {
        SetupCloseInfFile(inf_handle);
        break;
      }
 
    dev_index = 0;

    /* enumerate the device list to find all attached and unattached */
    /* devices */
    while(SetupDiEnumDeviceInfo(dev_info, dev_index, &dev_info_data))
      {
        /* get the harware ID from the registry, this is a multi-zero string */
        if(SetupDiGetDeviceRegistryProperty(dev_info, &dev_info_data,
                                            SPDRP_HARDWAREID, NULL,  
                                            (BYTE *)tmp_id, 
                                            sizeof(tmp_id), NULL))
          {
            /* check all possible IDs contained in that multi-zero string */
            for(p = tmp_id; *p; p += (strlen(p) + 1))
              {
                /* convert the string to lowercase */
                strlwr(p);
		
                /* found a match? */
                if(strstr(p, id))
                  {
                    /* is this device disconnected? */
                    if(CM_Get_DevNode_Status(&status,
                                             &problem,
                                             dev_info_data.DevInst,
                                             0) == CR_NO_SUCH_DEVINST)
                      {
                        /* found a device node that represents an unattached */
                        /* device */
                        if(SetupDiGetDeviceRegistryProperty(dev_info, 
                                                            &dev_info_data,
                                                            SPDRP_CONFIGFLAGS, 
                                                            NULL,  
                                                            (BYTE *)&config_flags, 
                                                            sizeof(config_flags),
                                                            NULL))
                          {
                            /* mark the device to be reinstalled the next time it is */
                            /* plugged in */
                            config_flags |= CONFIGFLAG_REINSTALL;
			    
                            /* write the property back to the registry */
                            SetupDiSetDeviceRegistryProperty(dev_info, 
                                                             &dev_info_data,
                                                             SPDRP_CONFIGFLAGS,
                                                             (BYTE *)&config_flags, 
                                                             sizeof(config_flags));
                          }
                      }
                    /* a match was found, skip the rest */
                    break;
                  }
              }
          }
        /* check the next device node */
        dev_index++;
      }
    
    SetupDiDestroyDeviceInfoList(dev_info);

    /* get the next device ID from the .inf file */ 
  } while(SetupFindNextLine(&inf_context, &inf_context));

  /* we are done, close the .inf file */
  SetupCloseInfFile(inf_handle);

  usb_registry_stop_libusb_devices(); /* stop all libusb devices */
  usb_registry_start_libusb_devices(); /* restart all libusb devices */

  return 0;
}

/* usb-wdm-free: libusb-win32 1.x's installer entry points (design 2.5, */
/* 6.4). 1.x installs a driver package with them on 2000 and later; */
/* this DLL is for 98 SE / ME, where a device is installed from */
/* LIBUSB0.INF, so they install nothing and return -1. */
int usb_install_npW(HWND hwnd, HINSTANCE instance, LPCWSTR cmd_line,
                    int starg_arg)
{
  usb_error("usb_install_npW(): not supported on this system; "
            "install a device with LIBUSB0.INF");
  return -1;
}

int usb_install_npA(HWND hwnd, HINSTANCE instance, LPCSTR cmd_line,
                    int starg_arg)
{
  usb_error("usb_install_npA(): not supported on this system; "
            "install a device with LIBUSB0.INF");
  return -1;
}

void CALLBACK usb_install_np_rundll(HWND wnd, HINSTANCE instance,
                                   LPSTR cmd_line, int cmd_show)
{
  usb_install_npA(wnd, instance, cmd_line, cmd_show);
}

void CALLBACK usb_touch_inf_file_np_rundll(HWND wnd, HINSTANCE instance,
                                           LPSTR cmd_line, int cmd_show)
{
  usb_touch_inf_file_np(cmd_line);
}

int usb_touch_inf_file_np(const char *inf_file)
{
  const char inf_comment[] = ";added by libusb to break this file's digital "
    "signature";
  const wchar_t inf_comment_uni[] = L";added by libusb to break this file's "
    L"digital signature";

  char buf[1024];
  wchar_t wbuf[1024];
  int found = 0;
  OSVERSIONINFO version;
  FILE *f;

  version.dwOSVersionInfoSize = sizeof(OSVERSIONINFO);

  if(!GetVersionEx(&version))
    return -1;


  /* XP system */
  if((version.dwMajorVersion == 5) && (version.dwMinorVersion >= 1))
    {
      f = fopen(inf_file, "rb");
      
      if(!f)
        return -1;

      while(fgetws(wbuf, sizeof(wbuf)/2, f))
        {
          if(wcsstr(wbuf, inf_comment_uni))
            {
              found = 1;
              break;
            }
        }

      fclose(f);

      if(!found)
        {
          f = fopen(inf_file, "ab");
          /*           fputwc(0x000d, f); */
          /*           fputwc(0x000d, f); */
          fputws(inf_comment_uni, f);
          fclose(f);
        }
    }
  else
    {
      f = fopen(inf_file, "r");
      
      if(!f)
        return -1;

      while(fgets(buf, sizeof(buf), f))
        {
          if(strstr(buf, inf_comment))
            {
              found = 1;
              break;
            }
        }

      fclose(f);

      if(!found)
        {
          f = fopen(inf_file, "a");
          fputs("\n", f);
          fputs(inf_comment, f);
          fputs("\n", f);
          fclose(f);
        }
    }

  return 0;
}

int usb_install_needs_restart_np(void)
{
  HDEVINFO dev_info;
  SP_DEVINFO_DATA dev_info_data;
  int dev_index = 0;
  SP_DEVINSTALL_PARAMS install_params;
  int ret = FALSE;

  dev_info_data.cbSize = sizeof(SP_DEVINFO_DATA);
  dev_info = SetupDiGetClassDevs(NULL, NULL, NULL,
                                 DIGCF_ALLCLASSES | DIGCF_PRESENT);
  
  SetEnvironmentVariable("LIBUSB_NEEDS_REBOOT", "1");

  if(dev_info == INVALID_HANDLE_VALUE)
    {
      usb_error("usb_install_needs_restart_np(): getting "
                "device info set failed");
      return ret;
    }
  
  while(SetupDiEnumDeviceInfo(dev_info, dev_index, &dev_info_data))
    {
      memset(&install_params, 0, sizeof(SP_PROPCHANGE_PARAMS));
      install_params.cbSize = sizeof(SP_DEVINSTALL_PARAMS);

      if(SetupDiGetDeviceInstallParams(dev_info, &dev_info_data, 
                                       &install_params))
        {
          if(install_params.Flags & (DI_NEEDRESTART | DI_NEEDREBOOT))
            {
              usb_message("usb_install_needs_restart_np(): restart needed");
              ret = TRUE;
            }
        }
      
      dev_index++;
    }
  
  SetupDiDestroyDeviceInfoList(dev_info);

  return ret;
}
