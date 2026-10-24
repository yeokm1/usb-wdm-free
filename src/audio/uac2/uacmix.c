// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *   (Tentative) USB Audio Driver for ALSA
 *   Mixer control part
 *   Copyright (c) 2002 by Takashi Iwai <tiwai@suse.de>
 *   Many codes borrowed from audio.c by
 *       Alan Cox (alan@lxorguk.ukuu.org.uk)
 *       Thomas Sailer (sailer@ife.ee.ethz.ch)
 *
 * Copyright (C) 2026 Yeo Kheng Meng
 * Adapted check_matrix_bitmap and UAC2 mixer_bitmap_overflow semantics from
 * Linux v6.12 sound/usb/mixer.c, commit
 * adc218676eef25575469234709c2d87185ca223a.
 * Original caller-buffer validation and standard UAC2 MCN request wrapper.
 * No ALSA/Linux runtime or build dependency. USB-IF A4.7.2.6/A5.2.5.5.
 */
#include "uacmix.h"
/* Linux bitmap ordering retained: rows are inputs, columns are outputs,
 * and the first matrix node is the MSB of the first byte, not its LSB. */
static int MatrixBit(const unsigned char *bitmap,unsigned int input,
    unsigned int output,unsigned int outputs)
{
    unsigned int index=input*outputs+output;
    return bitmap[index>>3]&(0x80>>(index&7));
}
static int Valid(const UAC_MIX *m)
{
    return m && m->Descriptor && m->Length<=255 && m->Length>=15 && m->Unit>=1 && m->Unit<=255 &&
        m->Pins>=1 && m->Pins<=255 && m->Inputs>=1 && m->Inputs<=256 && m->Outputs>=1 && m->Outputs<=255 &&
        m->Outputs<=256/m->Inputs && m->Cells==m->Inputs*m->Outputs &&
        m->MatrixOffset==11+m->Pins && m->MatrixBytes==(m->Cells+7)/8 &&
        m->MatrixOffset+m->MatrixBytes+2==m->Length;
}
int UacMixParse(const unsigned char *d,unsigned int available,
    const unsigned int *pinChannels,unsigned int pinCount,UAC_MIX *out)
{
    UAC_MIX m;unsigned int i,inputs=0,pins,outputs,cells,offset,count,controls;
    if(!d || !out || !pinChannels || available<5 || d[0]<5 || d[0]>available ||
       d[1]!=0x24 || d[2]!=4 || !d[3] || !d[4]) return 0;
    pins=d[4];if(pinCount!=pins || d[0]<13+pins) return 0;
    for(i=0;i<pins;i++) {
        if(!d[5+i] || d[5+i]==d[3] || !pinChannels[i] || pinChannels[i]>255 || pinChannels[i]>256-inputs) return 0;
        inputs+=pinChannels[i];
    }
    outputs=d[5+pins];
    /* Linux's remaining-tail calculation uses2 bytes for UAC2. Add the class
     * limit of256 nodes, checked BEFORE multiplication and pointer arithmetic. */
    if(!outputs || outputs>256/inputs) return 0;
    cells=inputs*outputs;offset=11+pins;count=(cells+7)/8;
    if(offset+count+2!=d[0]) return 0;
    if((cells&7) && (d[offset+count-1]&((1u<<(8-(cells&7)))-1))) return 0;
    controls=d[offset+count];if(controls&0xc0) return 0;
    for(i=0;i<3;i++) if(((controls>>(2*i))&3)==2) return 0;
    m.Descriptor=d;m.Length=d[0];m.Unit=d[3];m.Pins=pins;m.Inputs=inputs;m.Outputs=outputs;
    m.Cells=cells;m.MatrixOffset=offset;m.MatrixBytes=count;m.Controls=controls;
    m.ChannelConfig=0;for(i=0;i<4;i++) m.ChannelConfig|=(unsigned int)d[6+pins+i]<<(8*i);
    m.ChannelNames=d[10+pins];m.MixerName=d[offset+count+1];*out=m;return 1;
}
int UacMixControlNumber(const UAC_MIX *m,unsigned int input,unsigned int output,unsigned int *mcn)
{
    if(!mcn || !Valid(m) || input>=m->Inputs || output>=m->Outputs) return 0;
    *mcn=input*m->Outputs+output;return 1;
}
int UacMixAccess(const UAC_MIX *m,unsigned int input,unsigned int output)
{
    unsigned int mcn;
    if(!UacMixControlNumber(m,input,output,&mcn)) return 0;
    return MatrixBit(m->Descriptor+m->MatrixOffset,input,output,m->Outputs)?3:1;
}
int UacMixSetup(const UAC_MIX *m,unsigned int interfaceNumber,
    unsigned int input,unsigned int output,unsigned int attribute,unsigned int read,
    unsigned int length,unsigned int rangeWritable,unsigned char setup[8])
{
    unsigned int mcn;int access;UAC_CTL_TARGET target;
    if(!UacMixControlNumber(m,input,output,&mcn)) return 0;
    access=UacMixAccess(m,input,output);
    target.Interface=interfaceNumber;target.Entity=m->Unit;target.Endpoint=0;
    target.Channel=mcn;target.MaxChannel=m->Cells-1;
    return UacCtlSetup(UAC_CTL_MIXER,1,&target,(unsigned int)access,attribute,read,length,rangeWritable,setup);
}
