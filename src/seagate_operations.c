//
// Do NOT modify or remove this copyright and license
//
// Copyright (c) 2012 - 2017 Seagate Technology LLC and/or its Affiliates, All Rights Reserved
//
// This software is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.
//
// ******************************************************************************************
// 
// \file seagate_operations.c
// \brief This file defines the functions for Seagate drive specific operations that are customer safe

#include "seagate_operations.h"
#include "logs.h"
#include "smart.h"
#include "sector_repair.h"
#include "dst.h"
#include "sanitize.h"
#include "format_unit.h"

int seagate_ata_SCT_SATA_phy_speed(tDevice *device, bool useGPL, bool useDMA, uint8_t speedGen)
{
    int ret = UNKNOWN;
    uint8_t *sctSATAPhySpeed = (uint8_t*)calloc(LEGACY_DRIVE_SEC_SIZE, sizeof(uint8_t));
    if (sctSATAPhySpeed == NULL)
    {
        perror("Calloc Failure!\n");
        return MEMORY_FAILURE;
    }
    //speedGen = 1 means generation 1 (1.5Gb/s), 2 =  2nd Generation (3.0Gb/s), 3 = 3rd Generation (6.0Gb/s)
    if (speedGen > 3)
    {
        safe_Free(sctSATAPhySpeed);
        return BAD_PARAMETER;
    }

    //fill in the buffer with the correct information
    //action code
    sctSATAPhySpeed[0] = (uint8_t)SCT_SEAGATE_SPEED_CONTROL;
    sctSATAPhySpeed[1] = (uint8_t)(SCT_SEAGATE_SPEED_CONTROL >> 8);
    //function code
    sctSATAPhySpeed[2] = (uint8_t)BIST_SET_SATA_PHY_SPEED;
    sctSATAPhySpeed[3] = (uint8_t)(BIST_SET_SATA_PHY_SPEED >> 8);
    //feature code
    sctSATAPhySpeed[4] = RESERVED;
    sctSATAPhySpeed[5] = RESERVED;
    //state
    sctSATAPhySpeed[6] = RESERVED;
    sctSATAPhySpeed[7] = RESERVED;
    //option flags
    sctSATAPhySpeed[8] = 0x01;//save feature to disk...I'm assuming this is always what's wanted since the other flags don't make sense since this feature requires a power cycle
    sctSATAPhySpeed[9] = 0x00;
    //reserved words are from byte 10:27
    //the data transferspeed goes in bytes 28 and 29, although byte 28 will be zero
    sctSATAPhySpeed[28] = speedGen;
    sctSATAPhySpeed[29] = 0x00;

    ret = ata_SCT_Command(device, useGPL, useDMA, sctSATAPhySpeed, LEGACY_DRIVE_SEC_SIZE, false);

    safe_Free(sctSATAPhySpeed);
    return ret;
}

typedef enum _eSASPhySpeeds
{
    SAS_PHY_NO_CHANGE = 0x0,
    //reserved
    SAS_PHY_1_5_Gbs = 0x8,
    SAS_PHY_3_Gbs = 0x9,
    SAS_PHY_6_Gbs = 0xA,
    SAS_PHY_12_Gbs = 0xB,
    SAS_PHY_22_5_Gbs = 0xC,
    //values Dh - Fh are reserved for future speeds
}eSASPhySpeeds;

//valid phySpeedGen values are 1 - 5. This will need to be modified if SAS get's higher link rates than 22.5Gb/s
int scsi_Set_Phy_Speed(tDevice *device, uint8_t phySpeedGen, bool allPhys, uint8_t phyNumber)
{
    int ret = SUCCESS;
    if (phySpeedGen > 5)
    {
        return NOT_SUPPORTED;
    }
    uint16_t phyControlLength = 104;//size of 104 comes from 8 byte page header + (2 * 48bytes) for 2 phy descriptors. This is assuming drives only have 2...which is true right now, but the code will detect when it needs to reallocate and read more from the drive.
    uint8_t *sasPhyControl = (uint8_t*)calloc((MODE_PARAMETER_HEADER_10_LEN + phyControlLength) * sizeof(uint8_t), sizeof(uint8_t));
    if (!sasPhyControl)
    {
        return MEMORY_FAILURE;
    }
    if (SUCCESS == scsi_Mode_Sense_10(device, MP_PROTOCOL_SPECIFIC_PORT, (MODE_PARAMETER_HEADER_10_LEN + phyControlLength), 0x01, true, true, MPC_CURRENT_VALUES, sasPhyControl))
    {
        //make sure we got the header as we expect it, then validate we got all the data we needed.
        uint16_t modeDataLength = M_BytesTo2ByteValue(sasPhyControl[0], sasPhyControl[1]);
        uint16_t blockDescriptorLength = M_BytesTo2ByteValue(sasPhyControl[6], sasPhyControl[7]);
        //validate we got the right page
        if ((sasPhyControl[MODE_PARAMETER_HEADER_10_LEN + blockDescriptorLength + 0] & 0x3F) == 0x19 && (sasPhyControl[MODE_PARAMETER_HEADER_10_LEN + blockDescriptorLength + 1]) == 0x01 && (sasPhyControl[MODE_PARAMETER_HEADER_10_LEN + blockDescriptorLength + 0] & BIT6) > 0)
        {
            uint16_t pageLength = M_BytesTo2ByteValue(sasPhyControl[MODE_PARAMETER_HEADER_10_LEN + blockDescriptorLength + 2], sasPhyControl[MODE_PARAMETER_HEADER_10_LEN + blockDescriptorLength + 3]);
            //check that we were able to read the full page! If we didn't get the entire thing, we need to reread it and adjust the phyControlLength variable!
            if ((pageLength + MODE_PARAMETER_HEADER_10_LEN) > modeDataLength || pageLength > phyControlLength)
            {
                //reread the page for the larger length
                phyControlLength = pageLength + 8 + MODE_PARAMETER_HEADER_10_LEN;
                uint8_t *temp = realloc(sasPhyControl, phyControlLength * sizeof(uint8_t));
                if (!temp)
                {
                    return MEMORY_FAILURE;
                }
                sasPhyControl = temp;
                if (SUCCESS != scsi_Mode_Sense_10(device, MP_PROTOCOL_SPECIFIC_PORT, (MODE_PARAMETER_HEADER_10_LEN + phyControlLength), 0x01, true, true, MPC_CURRENT_VALUES, sasPhyControl))
                {
                    safe_Free(sasPhyControl);
                    return FAILURE;
                }
            }
            if ((sasPhyControl[MODE_PARAMETER_HEADER_10_LEN + blockDescriptorLength + 5] & 0x0F) == 6)//make sure it's the SAS protocol page
            {
                uint8_t numberOfPhys = sasPhyControl[MODE_PARAMETER_HEADER_10_LEN + blockDescriptorLength + 7];
                uint32_t phyDescriptorOffset = MODE_PARAMETER_HEADER_10_LEN + blockDescriptorLength + 8;//this will be set to the beginnging of the phy descriptors so that when looping through them, it is easier code to read.
                for (uint16_t phyIter = 0; phyIter < (uint16_t)numberOfPhys; ++phyIter, phyDescriptorOffset += 48)
                {
                    uint8_t phyIdentifier = sasPhyControl[phyDescriptorOffset + 1];
                    //check if the caller requested changing all phys or a specific phy and only modify it's descriptor if either of those are true.
                    if (allPhys || phyNumber == phyIdentifier)
                    {
                        uint8_t hardwareMaximumLinkRate = M_Nibble0(sasPhyControl[phyDescriptorOffset + 33]);
                        if (phySpeedGen == 0)
                        {
                            //they want it back to default, so read the hardware maximum physical link rate and set it to the programmed maximum
                            uint8_t matchedRate = (hardwareMaximumLinkRate << 4) | hardwareMaximumLinkRate;
                            sasPhyControl[phyDescriptorOffset + 33] = matchedRate;
                        }
                        else
                        {
                            //they are requesting a specific speed, so set the value in the page.
                            switch (phySpeedGen)
                            {
                            case 1://1.5 Gb/s
                                sasPhyControl[phyDescriptorOffset + 33] = (SAS_PHY_1_5_Gbs << 4) | hardwareMaximumLinkRate;
                                break;
                            case 2://3.0 Gb/s
                                sasPhyControl[phyDescriptorOffset + 33] = (SAS_PHY_3_Gbs << 4) | hardwareMaximumLinkRate;
                                break;
                            case 3://6.0 Gb/s
                                sasPhyControl[phyDescriptorOffset + 33] = (SAS_PHY_6_Gbs << 4) | hardwareMaximumLinkRate;
                                break;
                            case 4://12.0 Gb/s
                                sasPhyControl[phyDescriptorOffset + 33] = (SAS_PHY_12_Gbs << 4) | hardwareMaximumLinkRate;
                                break;
                            case 5://22.5 Gb/s
                                sasPhyControl[phyDescriptorOffset + 33] = (SAS_PHY_22_5_Gbs << 4) | hardwareMaximumLinkRate;
                                break;
                            default:
                                //error! should be caught above!
                                break;
                            }
                        }
                    }
                }
                //we've finished making our changes to the mode page, so it's time to write it back!
                if (SUCCESS != scsi_Mode_Select_10(device, (MODE_PARAMETER_HEADER_10_LEN + phyControlLength), false, true, sasPhyControl, (MODE_PARAMETER_HEADER_10_LEN + phyControlLength)))
                {
                    ret = FAILURE;
                }
            }
            else
            {
                ret = NOT_SUPPORTED;
            }
        }
        else
        {
            ret = FAILURE;
        }
    }
    else
    {
        ret = FAILURE;
    }
    safe_Free(sasPhyControl);
    return ret;
}

int set_phy_speed(tDevice *device, uint8_t phySpeedGen, bool allPhys, uint8_t phyIdentifier)
{
    int ret = UNKNOWN;
    if (device->drive_info.drive_type == ATA_DRIVE)
    {
        if (is_Seagate_Family(device) == SEAGATE)
        {
            if (device->drive_info.IdentifyData.ata.Word206 & BIT7 && is_SSD(device) == false)
            {
                if (phySpeedGen > 3)
                {
                    //error, invalid input
                    if (VERBOSITY_QUIET < g_verbosity)
                    {
                        printf("Invalid PHY generation speed input. Please use 0 - 3.\n");
                    }
                    return BAD_PARAMETER;
                }
                ret = seagate_ata_SCT_SATA_phy_speed(device, device->drive_info.ata_Options.generalPurposeLoggingSupported, device->drive_info.ata_Options.dmaSupported, phySpeedGen);
            }
            else
            {
                ret = NOT_SUPPORTED;
            }
        }
        else
        {
            if (VERBOSITY_QUIET < g_verbosity)
            {
                printf("Setting the PHY speed of a device is only available on Seagate Drives.\n");
            }
            ret = NOT_SUPPORTED;
        }
    }
    else if (device->drive_info.drive_type == SCSI_DRIVE)
    {
        //call the scsi/sas function to set the phy speed.
        ret = scsi_Set_Phy_Speed(device, phySpeedGen, allPhys, phyIdentifier);
    }
    else
    {
        ret = NOT_SUPPORTED;
    }
    return ret;
}

bool is_Low_Current_Spin_Up_Enabled(tDevice *device)
{
    bool lowPowerSpinUpEnabled = false;
    if (device->drive_info.drive_type == ATA_DRIVE && is_Seagate_Family(device) == SEAGATE)
    {
        int ret = NOT_SUPPORTED;
        //first try the SCT feature control command to get it's state
        if (device->drive_info.IdentifyData.ata.Word206 & BIT4)
        {
            uint16_t optionFlags = 0x0000;
            uint16_t state = 0x0000;
            if (SUCCESS == ata_SCT_Feature_Control(device, device->drive_info.ata_Options.generalPurposeLoggingSupported, device->drive_info.ata_Options.readLogWriteLogDMASupported, 0x0002, 0xD001, &state, &optionFlags))
            {
                if (state > 0 && state < 3)//if the state is not 1 or 2, then we have an unknown value being given to us.
                {
                    ret = SUCCESS;
                    if (state == 0x0001)
                    {
                        lowPowerSpinUpEnabled = true;
                    }
                    else if (state == 0x0002)
                    {
                        lowPowerSpinUpEnabled = false;
                    }
                }
            }
        }
        if(ret != SUCCESS) //check the identify data for a bit (2.5" drives only I think) - TJE
        {
            //refresh Identify data
            ata_Identify(device, (uint8_t*)&device->drive_info.IdentifyData.ata.Word000, LEGACY_DRIVE_SEC_SIZE);
            if (device->drive_info.IdentifyData.ata.Word155 & BIT1)
            {
                lowPowerSpinUpEnabled = true;
            }
        }
    }
    return lowPowerSpinUpEnabled;
}

int enable_Low_Current_Spin_Up(tDevice *device)
{
    int ret = NOT_SUPPORTED;
    if (device->drive_info.drive_type == ATA_DRIVE)
    {
        //first try the SCT feature control command
        if (device->drive_info.IdentifyData.ata.Word206 & BIT4)
        {
            uint16_t saveToDrive = 0x0001;
            uint16_t state = 0x0001;
            if (SUCCESS == ata_SCT_Feature_Control(device, device->drive_info.ata_Options.generalPurposeLoggingSupported, device->drive_info.ata_Options.readLogWriteLogDMASupported, 0x0001, 0xD001, &state, &saveToDrive))
            {
                ret = SUCCESS;
            }
        }
        if (ret != SUCCESS) //try the Seagate unique set features (I think this is only on newer 2.5" drives) - TJE
        {
            if (SUCCESS == ata_Set_Features(device, 0x5B, 0, 0x01, 0xED, 0x00B5))
            {
                ret = SUCCESS;
            }
        }
    }
    return ret;
}

int disable_Low_Current_Spin_Up(tDevice *device)
{
    int ret = NOT_SUPPORTED;
    if (device->drive_info.drive_type == ATA_DRIVE)
    {
        //first try the SCT feature control command
        if (device->drive_info.IdentifyData.ata.Word206 & BIT4)
        {
            uint16_t saveToDrive = 0x0001;
            uint16_t state = 0x0002;
            if (SUCCESS == ata_SCT_Feature_Control(device, device->drive_info.ata_Options.generalPurposeLoggingSupported, device->drive_info.ata_Options.readLogWriteLogDMASupported, 0x0001, 0xD001, &state, &saveToDrive))
            {
                ret = SUCCESS;
            }
        }
        if(ret != SUCCESS) //try the Seagate unique set features (I think this is only on newer 2.5" drives) - TJE
        {
            if (SUCCESS == ata_Set_Features(device, 0x5B, 0, 0x00, 0xED, 0x00B5))
            {
                ret = SUCCESS;
            }
        }
    }
    return ret;
}

int set_SSC_Feature_SATA(tDevice *device, eSSCFeatureState mode)
{
    int ret = NOT_SUPPORTED;
    if (device->drive_info.drive_type == ATA_DRIVE)
    {
        if (is_Seagate_Family(device) == SEAGATE)
        {
            if (device->drive_info.IdentifyData.ata.Word206 & BIT4)
            {
                uint16_t state = (uint16_t)mode;
                uint16_t saveToDrive = 0x0001;
                if (SUCCESS == ata_SCT_Feature_Control(device, device->drive_info.ata_Options.generalPurposeLoggingSupported, device->drive_info.ata_Options.readLogWriteLogDMASupported, 0x0001, 0xD002, &state, &saveToDrive))
                {
                    ret = SUCCESS;
                }
                else
                {
                    ret = FAILURE;
                }
            }
        }
        else
        {
            if (VERBOSITY_QUIET < g_verbosity)
            {
                printf("Setting the SSC feature of a device is only available on Seagate Drives.\n");
            }
            ret = NOT_SUPPORTED;
        }
    }
    return ret;
}

int get_SSC_Feature_SATA(tDevice *device, eSSCFeatureState *mode)
{
    int ret = NOT_SUPPORTED;
    if (device->drive_info.drive_type == ATA_DRIVE)
    {
        if (is_Seagate_Family(device) == SEAGATE)
        {
            if (device->drive_info.IdentifyData.ata.Word206 & BIT4)
            {
                uint16_t state = 0;
                uint16_t saveToDrive = 0;
                if (SUCCESS == ata_SCT_Feature_Control(device, device->drive_info.ata_Options.generalPurposeLoggingSupported, device->drive_info.ata_Options.readLogWriteLogDMASupported, 0x0002, 0xD002, &state, &saveToDrive))
                {
                    ret = SUCCESS;
                    *mode = (eSSCFeatureState)state;
                }
                else
                {
                    ret = FAILURE;
                }
            }
        }
        else
        {
            if (VERBOSITY_QUIET < g_verbosity)
            {
                printf("Getting the SSC feature of a device is only available on Seagate Drives.\n");
            }
            ret = NOT_SUPPORTED;
        }
    }
    return ret;
}

int seagate_SAS_Get_JIT_Modes(tDevice *device, ptrSeagateJITModes jitModes)
{
    int ret = NOT_SUPPORTED;
    eSeagateFamily family = is_Seagate_Family(device);
    if (!jitModes)
    {
        return BAD_PARAMETER;
    }
    if (family == SEAGATE || family == SEAGATE_VENDOR_A)
    {
        if (!is_SSD(device))
        {
            //HDD, so we can do this.
            uint8_t seagateUnitAttentionParameters[12 + MODE_PARAMETER_HEADER_10_LEN] = { 0 };
            bool readPage = false;
            uint8_t headerLength = MODE_PARAMETER_HEADER_10_LEN;
            if (SUCCESS == scsi_Mode_Sense_10(device, 0, 12 + MODE_PARAMETER_HEADER_10_LEN, 0, true, false, MPC_CURRENT_VALUES, seagateUnitAttentionParameters))
            {
                readPage = true;
            }
            else if (SUCCESS == scsi_Mode_Sense_6(device, 0, 12 + MODE_PARAMETER_HEADER_6_LEN, 0, true, MPC_CURRENT_VALUES, seagateUnitAttentionParameters))
            {
                readPage = true;
                headerLength = MODE_PARAMETER_HEADER_6_LEN;
            }
            if (readPage)
            {
                ret = SUCCESS;
                jitModes->valid = true;
                if (seagateUnitAttentionParameters[headerLength + 4] & BIT7)//vjit disabled
                {
                    jitModes->vJIT = true;
                }
                else
                {
                    jitModes->vJIT = false;
                }
                if (seagateUnitAttentionParameters[headerLength + 4] & BIT3)
                {
                    jitModes->jit3 = true;
                }
                if (seagateUnitAttentionParameters[headerLength + 4] & BIT2)
                {
                    jitModes->jit2 = true;
                }
                if (seagateUnitAttentionParameters[headerLength + 4] & BIT1)
                {
                    jitModes->jit1 = true;
                }
                if (seagateUnitAttentionParameters[headerLength + 4] & BIT0)
                {
                    jitModes->jit0 = true;
                }
            }
            else
            {
                ret = FAILURE;//Or not supported??
            }
        }
    }
    return ret;
}

int seagate_Get_JIT_Modes(tDevice *device, ptrSeagateJITModes jitModes)
{
    int ret = NOT_SUPPORTED;
    if (device->drive_info.drive_type == SCSI_DRIVE)
    {
        ret = seagate_SAS_Get_JIT_Modes(device, jitModes);
    }
    return ret;
}

int seagate_SAS_Set_JIT_Modes(tDevice *device, bool disableVjit, uint8_t jitMode, bool revertToDefaults, bool nonvolatile)
{
    int ret = NOT_SUPPORTED;
    eSeagateFamily family = is_Seagate_Family(device);
    if (family == SEAGATE || family == SEAGATE_VENDOR_A)
    {
        if (!is_SSD(device))
        {
            //HDD, so we can do this.
            uint8_t seagateUnitAttentionParameters[12 + MODE_PARAMETER_HEADER_10_LEN] = { 0 };
            bool readPage = false;
            uint8_t headerLength = MODE_PARAMETER_HEADER_10_LEN;
            if (revertToDefaults)
            {
                //We need to read the default mode page to get the status of the JIT bits, save them, then pass them along...
                bool readDefaults = false;
                if (SUCCESS == scsi_Mode_Sense_10(device, 0, 12 + MODE_PARAMETER_HEADER_10_LEN, 0, true, false, MPC_DEFAULT_VALUES, seagateUnitAttentionParameters))
                {
                    readDefaults = true;
                    headerLength = MODE_PARAMETER_HEADER_10_LEN;
                }
                else if (SUCCESS == scsi_Mode_Sense_6(device, 0, 12 + MODE_PARAMETER_HEADER_6_LEN, 0, true, MPC_DEFAULT_VALUES, seagateUnitAttentionParameters))
                {
                    readDefaults = true;
                    headerLength = MODE_PARAMETER_HEADER_6_LEN;
                }
                if (readDefaults)
                {
                    if (seagateUnitAttentionParameters[headerLength + 4] & BIT7)//vjit disabled
                    {
                        disableVjit = false;
                    }
                    else
                    {
                        disableVjit = true;
                    }
                    if (seagateUnitAttentionParameters[headerLength + 4] & BIT0)
                    {
                        jitMode = 0;
                    }
                    else if (seagateUnitAttentionParameters[headerLength + 4] & BIT1)
                    {
                        jitMode = 1;
                    }
                    else if (seagateUnitAttentionParameters[headerLength + 4] & BIT2)
                    {
                        jitMode = 2;
                    }
                    else if (seagateUnitAttentionParameters[headerLength + 4] & BIT3)
                    {
                        jitMode = 3;
                    }
                }
                else
                {
                    return FAILURE;
                }
            }
            if (SUCCESS == scsi_Mode_Sense_10(device, 0, 12 + MODE_PARAMETER_HEADER_10_LEN, 0, true, false, MPC_CURRENT_VALUES, seagateUnitAttentionParameters))
            {
                readPage = true;
                headerLength = MODE_PARAMETER_HEADER_10_LEN;
            }
            else if (SUCCESS == scsi_Mode_Sense_6(device, 0, 12 + MODE_PARAMETER_HEADER_6_LEN, 0, true, MPC_CURRENT_VALUES, seagateUnitAttentionParameters))
            {
                readPage = true;
                headerLength = MODE_PARAMETER_HEADER_6_LEN;
            }
            if (readPage)
            {
                //We've read the page, so now we must set up the requested JIT modes
                seagateUnitAttentionParameters[headerLength + 4] &= 0x70;//clear all bits to zero, except 4, 5, & 6 since those are reserved (and if they are ever used, we don't want to touch them now) - TJE
                if (!disableVjit)
                {
                    seagateUnitAttentionParameters[headerLength + 4] |= BIT7;
                }
                switch (jitMode)//Spec says that faster modes allow the drive to continue accessing slower modes, so might as well set each bit below the requested mode.
                {
                default:
                case 0:
                    seagateUnitAttentionParameters[headerLength + 4] |= BIT0;
                case 1:
                    seagateUnitAttentionParameters[headerLength + 4] |= BIT1;
                case 2:
                    seagateUnitAttentionParameters[headerLength + 4] |= BIT2;
                case 3:
                    seagateUnitAttentionParameters[headerLength + 4] |= BIT3;
                }
                //Now we need to do a mode select to send this data back to the drive!!
                if (headerLength == MODE_PARAMETER_HEADER_10_LEN)
                {
                    ret = scsi_Mode_Select_10(device, 12 + headerLength, false, nonvolatile, seagateUnitAttentionParameters, 12 + headerLength);
                }
                else
                {
                    ret = scsi_Mode_Select_6(device, 12 + headerLength, false, nonvolatile, seagateUnitAttentionParameters, 12 + headerLength);
                }
            }
            else
            {
                ret = FAILURE;//Or not supported??
            }
        }
    }
    return ret;
}

int seagate_Set_JIT_Modes(tDevice *device, bool disableVjit, uint8_t jitMode, bool revertToDefaults, bool nonvolatile)
{
    int ret = NOT_SUPPORTED;
    if (device->drive_info.drive_type == SCSI_DRIVE)
    {
        ret = seagate_SAS_Set_JIT_Modes(device, disableVjit, jitMode, revertToDefaults, nonvolatile);
    }
    return ret;
}

int seagate_Get_Power_Balance(tDevice *device, bool *supported, bool *enabled)
{
    int ret = NOT_SUPPORTED;
    if (device->drive_info.drive_type == ATA_DRIVE)
    {
        if (is_Seagate_Family(device) == SEAGATE)
        {
            ret = SUCCESS;
            if (supported)
            {
                //BIT8 for older products with this feature. EX: ST10000NM*
                //Bit10 for newwer products with this feature. EX: ST12000NM*
                if (device->drive_info.IdentifyData.ata.Word149 & BIT8 || device->drive_info.IdentifyData.ata.Word149 & BIT10)
                {
                    *supported = true;
                }
                else
                {
                    *supported = false;
                }
            }
            if (enabled)
            {
                //BIT9 for older products with this feature. EX: ST10000NM*
                //Bit11 for newwer products with this feature. EX: ST12000NM*
                if (device->drive_info.IdentifyData.ata.Word149 & BIT9 || device->drive_info.IdentifyData.ata.Word149 & BIT11)
                {
                    *enabled = true;
                }
                else
                {
                    *enabled = false;
                }
            }
        }
    }
    //SAS is NOT in here since it uses the same field as the set power consumption stuff from SPC. That should be used instead to set the drive's power consumption level.
    return ret;
}

int seagate_Set_Power_Balance(tDevice *device, bool enable)
{
    int ret = NOT_SUPPORTED;
    if (device->drive_info.drive_type == ATA_DRIVE)
    {
        if (enable)
        {
            ret = ata_Set_Features(device, 0x5C, 0, 1, 0, 0);
        }
        else
        {
            ret = ata_Set_Features(device, 0x5C, 0, 2, 0, 0);
        }
    }
    return ret;
}