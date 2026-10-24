/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Yeo Kheng Meng
 * Original tests: exact recorded descriptor fixtures plus labelled synthetic
 * USB2 descriptors/control replies. No USB access or device register writes.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "axctrl.h"
static unsigned long checks;
static void Check(int value,unsigned int line,const char *expr)
{ ++checks;if(!value) {printf("FAIL line%u: %s\n",line,expr);exit(1);} }
#define CHECK(x) Check((x),__LINE__,#x)
static unsigned char hostDev[18],guestDev[18],hostCfg[256],guestCfg[256],hsDev[18],hsCfg[256],changed[256];
static unsigned int Load(const char *root,const char *relative,unsigned char *out,unsigned int capacity)
{
    FILE *f;char path[4096];unsigned int n;int extra;
    CHECK(strlen(root)+strlen(relative)+2<sizeof(path));sprintf(path,"%s/%s",root,relative);
    f=fopen(path,"rb");CHECK(f!=NULL);n=fread(out,1,capacity,f);extra=fgetc(f);fclose(f);CHECK(extra==EOF);return n;
}
static unsigned int Usb2(unsigned int hostLength,unsigned int packet)
{
    unsigned int o,n=0,size;
    memcpy(hsDev,hostDev,18);hsDev[2]=0;hsDev[3]=2;hsDev[7]=64;
    for(o=0;o<hostLength;o+=size) {
        size=hostCfg[o];if(hostCfg[o+1]==0x30) continue;
        memcpy(hsCfg+n,hostCfg+o,size);
        if(hsCfg[n+1]==5 && (hsCfg[n+3]&3)==2) {hsCfg[n+4]=(unsigned char)packet;hsCfg[n+5]=(unsigned char)(packet>>8);}
        n+=size;
    }
    hsCfg[2]=(unsigned char)n;hsCfg[3]=(unsigned char)(n>>8);return n;
}
int main(int argc,char **argv)
{
    AX_CTRL_DEVICE d,ss;
    AX_CTRL_PLAN plan;
    AX_CTRL_LINK link;
    AX_CTRL_LINK_PLAN parameters;
    unsigned char setup[10],phy[2],usb,mac[6]={2,0x11,0x22,0x33,0x44,0x55};
    unsigned char *pages,*end;
    SYSTEM_INFO info;DWORD old;
    unsigned int hn,gn,n,i,j,k,duplex,rate,speed,expectedMedium;
    static const unsigned int speeds[]={AX_SPEED_FULL,AX_SPEED_HIGH,AX_SPEED_SUPER};
    CHECK(argc==2);
    CHECK(Load(argv[1],"src/net/ax88179/testdata/ax179/device.bin",hostDev,18)==18);
    CHECK(Load(argv[1],"src/net/ax88179/testdata/ax2k/device.bin",guestDev,18)==18);
    hn=Load(argv[1],"src/net/ax88179/testdata/ax179/config0.bin",hostCfg,256);
    gn=Load(argv[1],"src/net/ax88179/testdata/ax2k/config0.bin",guestCfg,256);
    CHECK(hn==57 && gn==57);
    CHECK(AxCtrlDescribe(hostDev,18,hostCfg,hn,AX_SPEED_SUPER,&ss)==AX_CTRL_OK);
    CHECK(ss.Valid && ss.InPacket==1024 && ss.OutPacket==1024 && ss.InBurst==4 && ss.OutBurst==16);
    CHECK(ss.Warnings==AX_WARN_STATUS_INTERVAL_ZERO && ss.StatusBytes==0);
    CHECK(AxCtrlDescribe(hostDev,18,hostCfg,hn,AX_SPEED_HIGH,&d)==AX_CTRL_SPEED_MISMATCH && !d.Valid);
    CHECK(AxCtrlDescribe(guestDev,18,guestCfg,gn,AX_SPEED_HIGH,&d)==AX_CTRL_SPEED_MISMATCH && !d.Valid);
    CHECK(AxCtrlDescribe(guestDev,18,guestCfg,gn,AX_SPEED_SUPER,&d)==AX_CTRL_SPEED_MISMATCH && !d.Valid);
    n=Usb2(hn,512);CHECK(n==39);
    CHECK(AxCtrlDescribe(hsDev,18,hsCfg,n,AX_SPEED_HIGH,&d)==AX_CTRL_OK && d.InPacket==512 && d.InBurst==1);
    CHECK(AxCtrlDescribe(hsDev,18,hsCfg,n,AX_SPEED_FULL,&d)==AX_CTRL_SPEED_MISMATCH);
    n=Usb2(hn,64);CHECK(AxCtrlDescribe(hsDev,18,hsCfg,n,AX_SPEED_FULL,&d)==AX_CTRL_OK && d.InPacket==64);
    CHECK(AxCtrlReadPlan(&d,&plan)==AX_CTRL_OK && plan.Valid && !plan.HasWrites && plan.Count==3);
    CHECK(AxCtrlSetup(&plan.Step[0],setup) && !memcmp(setup,"\xc0\x01\x02\0\x01\0\x01\0",8));
    memset(setup,0xaa,sizeof(setup));CHECK(AxCtrlSetup(&plan.Step[1],setup+1));
    CHECK(setup[0]==0xaa && setup[9]==0xaa && !memcmp(setup+1,"\xc0\x01\x10\0\x06\0\x06\0",8));
    CHECK(AxCtrlResponse(&plan.Step[1],6) && !AxCtrlResponse(&plan.Step[1],5));
    CHECK(!AxCtrlResponse(&plan.Step[1],7));
    CHECK(AxCtrlSetup(&plan.Step[2],setup) && !memcmp(setup,"\xc0\x02\x03\0\x11\0\x02\0",8));
    CHECK(AxCtrlPowerPrefix(&d,&plan)==AX_CTRL_OK && plan.HasWrites && plan.Count==6 && plan.NextGate==AX_GATE_AUTODETACH_DECISION);
    CHECK(plan.Step[1].Data[0]==0x20 && plan.Step[1].Data[1]==0 && plan.Step[2].DelayMs==500 && plan.Step[4].DelayMs==200);
    CHECK(AxCtrlSetup(&plan.Step[0],setup) && !memcmp(setup,"\x40\x01\x26\0\x02\0\x02\0",8));
    CHECK(AxCtrlSetup(&plan.Step[5],setup) && !memcmp(setup,"\xc0\x04\x43\0\x01\0\x02\0",8));
    CHECK(!AxCtrlSetup(&plan.Step[2],setup));
    plan.Step[0].Length=9;memset(setup,0xaa,8);CHECK(!AxCtrlSetup(&plan.Step[0],setup) && setup[0]==0xaa && setup[7]==0xaa);
    CHECK(AxCtrlMac(mac,6));mac[0]=1;CHECK(!AxCtrlMac(mac,6));memset(mac,0,6);CHECK(!AxCtrlMac(mac,6));
    memset(mac,255,6);CHECK(!AxCtrlMac(mac,6));mac[0]=2;CHECK(AxCtrlMac(mac,6));CHECK(!AxCtrlMac(mac,5));
    phy[0]=phy[1]=0;CHECK(AxCtrlPhy(phy,2,&link) && !link.Up);
    phy[0]=phy[1]=255;CHECK(!AxCtrlPhy(phy,2,&link) && !link.Up);
    phy[0]=0;phy[1]=0xc4;CHECK(!AxCtrlPhy(phy,2,&link));
    CHECK(!AxCtrlPhy(phy,1,&link));
    CHECK(!AxCtrlPhy(NULL,2,&link));CHECK(!AxCtrlMac(NULL,6));
    for(i=0;i<3;++i) for(rate=0;rate<3;++rate) for(duplex=0;duplex<2;++duplex) {
        speed=speeds[i];if(speed==AX_SPEED_SUPER) d=ss;
        else {n=Usb2(hn,speed==AX_SPEED_HIGH?512:64);CHECK(AxCtrlDescribe(hsDev,18,hsCfg,n,speed,&d)==0);}
        usb=(unsigned char)(speed==AX_SPEED_SUPER?4:speed==AX_SPEED_HIGH?2:0);
        phy[0]=0;phy[1]=(unsigned char)(4|(rate==2?0x80:rate==1?0x40:0)|(duplex?0x20:0));
        CHECK(AxCtrlLinkPlan(&d,mac,&usb,1,phy,2,&parameters)==0 && parameters.Valid && parameters.Link.Up);
        CHECK(parameters.Link.Megabits==(rate==2?1000U:rate==1?100U:10U) && parameters.Link.FullDuplex==duplex);
        expectedMedium=0x130|(rate==2?9:rate==1?0x200:0)|(duplex?2:0);
        CHECK(parameters.Medium==expectedMedium);
        CHECK(parameters.ReceiveBufferBytes==(rate==2 && speed==AX_SPEED_SUPER?20480U:rate==2 && speed==AX_SPEED_HIGH?24576U:26624U));
        CHECK(parameters.Queue[0]==7 && parameters.ReceiveBufferBytes%1024==0);
    }
    usb=2;CHECK(AxCtrlLinkPlan(&ss,mac,&usb,1,phy,2,&parameters)==AX_CTRL_SPEED_MISMATCH && !parameters.Valid);
    usb=4;phy[0]=phy[1]=0;CHECK(AxCtrlLinkPlan(&ss,mac,&usb,1,phy,2,&parameters)==0 && !parameters.Link.Up && !parameters.Medium && !parameters.ReceiveBufferBytes);
    d.Valid=0;CHECK(AxCtrlReadPlan(&d,&plan)==AX_CTRL_BAD && !plan.Valid && !plan.Count && !plan.HasWrites);
    CHECK(AxCtrlPowerPrefix(NULL,&plan)==AX_CTRL_BAD && !plan.Valid && !plan.Count && !plan.HasWrites);
    /* Guard-page reads of each exact/truncated recorded object. */
    GetSystemInfo(&info);pages=(unsigned char*)VirtualAlloc(NULL,info.dwPageSize*2,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
    CHECK(pages!=NULL);CHECK(VirtualProtect(pages+info.dwPageSize,info.dwPageSize,PAGE_NOACCESS,&old));end=pages+info.dwPageSize;
    for(i=0;i<18;++i) {memcpy(end-i,hostDev,i);CHECK(AxCtrlDescribe(end-i,i,hostCfg,hn,AX_SPEED_SUPER,&d)!=0 && !d.Valid);}
    for(i=0;i<hn;++i) {memcpy(end-i,hostCfg,i);CHECK(AxCtrlDescribe(hostDev,18,end-i,i,AX_SPEED_SUPER,&d)!=0 && !d.Valid);}
    CHECK(!AxCtrlPhy(end,0,&link));end[-1]=0;CHECK(!AxCtrlPhy(end-1,1,&link));
    end[-2]=0;CHECK(AxCtrlPhy(end-2,2,&link) && !link.Up);
    memcpy(end-hn,hostCfg,hn);CHECK(AxCtrlDescribe(hostDev,18,end-hn,hn,AX_SPEED_SUPER,&d)==0);
    for(i=0;i<hn;++i) for(j=1;j<=128;j<<=1) {
        memcpy(changed,hostCfg,hn);changed[i]^=(unsigned char)j;memcpy(end-hn,changed,hn);
        if(AxCtrlDescribe(hostDev,18,end-hn,hn,AX_SPEED_SUPER,&d)==0) CHECK(d.Valid && d.InPacket==1024 && d.OutPacket==1024 && d.StatusPacket==8);
        else CHECK(!d.Valid);
    }
    n=Usb2(hn,512);
    for(i=0;i<n;++i) for(j=1;j<=128;j<<=1) {
        memcpy(changed,hsCfg,n);changed[i]^=(unsigned char)j;
        if(AxCtrlDescribe(hsDev,18,changed,n,AX_SPEED_HIGH,&d)==0) CHECK(d.Valid && d.InPacket==512 && d.OutPacket==512);
        else CHECK(!d.Valid);
    }
    /* Exhaustive two-byte PHY input, invalid reserved encoding never accepted up. */
    for(k=0;k<65536;++k) {
        phy[0]=(unsigned char)k;phy[1]=(unsigned char)(k>>8);
        if(AxCtrlPhy(phy,2,&link)) CHECK(!link.Up || (link.Megabits==10 || link.Megabits==100 || link.Megabits==1000));
        else CHECK(!link.Up && !link.Megabits && !link.FullDuplex);
    }
    VirtualFree(pages,0,MEM_RELEASE);
    printf("PASS: %lu AX88179 descriptor/control/link checks (recorded SS/guest rejection, synthetic USB2, guards)\n",checks);
    return 0;
}
