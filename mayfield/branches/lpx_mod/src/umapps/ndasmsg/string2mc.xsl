﻿<?xml version="1.0" encoding="utf-8" ?>
<xsl:stylesheet xmlns:xsl="http://www.w3.org/1999/XSL/Transform" version="1.0">
<xsl:output method="text" encoding="unicode"/>
<!-- Override built-in template -->
<xsl:param name="lang">ENU</xsl:param>
<xsl:param name="module"></xsl:param>
<xsl:param name="type">header</xsl:param>

<xsl:template match="text()"/>
<xsl:template match="/"
>;/////////////////////////////////////////////////////////////////////////
;//
;// Copyright (C) 2004 XIMETA, Inc.
;// All rights reserved.
;//
;// Module Name:
;//
;//    _ndasmsg.h
;//
;// Abstract:
;//
;//    Messages for Event Logging for the service
;//
;//
;//Notes:
;//
;//	   DO NOT EDIT THIS FILE BY HAND!
;//
;//    This file is GENERATED by the xml file.
;//    and consequently by MC tool from the _ndasmsg.mc file.
;//
;//////////////////////////////////////////////////////////////////////////
;
;
;#ifndef __NDASMSG_H_
;#define __NDASMSG_H_
;
	
MessageIdTypeDef=DWORD

SeverityNames=(
Success=0x0:STATUS_SEVERITY_SUCCESS
Informational=0x1:STATUS_SEVERITY_INFORMATIONAL
Warning=0x2:STATUS_SEVERITY_WARNING
Error=0x3:STATUS_SEVERITY_ERROR
)

FacilityNames=(
NDASUSER=0x001
NDASCOMM=0x002
NDASOP=0x003
NDASPATCH=0x004
NDASSVC=0x010
NDASDM=0x110
EVT_NDASSVC=0xF00
)

LanguageNames=(
NEU=0000:_ndasmsg_loc_000
ENU=0x409:_ndasmsg_loc_409
CHS=0x804:_ndasmsg_loc_804
DEU=0x407:_ndasmsg_loc_407
ESN=0xC0A:_ndasmsg_loc_C0A
FRA=0x40C:_ndasmsg_loc_40C
ITA=0x410:_ndasmsg_loc_410
JPN=0x411:_ndasmsg_loc_411
KOR=0x412:_ndasmsg_loc_412
PTG=0x816:_ndasmsg_loc_816
)

<xsl:apply-templates/>

;#endif /* __NDASMSG_H_ */
</xsl:template>

<xsl:template match="messages">
	<!-- <xsl:if test="@module=$module and @langid=$lang" > -->
		<xsl:apply-templates select="message"/>
	<!-- </xsl:if> -->
</xsl:template>

<!--

- Sample XML data

<message
	id="0xFFFF" 
	symbolicName="NDAS_MSG_ERR_UNKNOWN" 
	severity="Error" 
	facility="Application">
<text language="ENU">Unknown Application Error</text>
<text language="KOR">알 수 없는 오류</text>
</message>

- Sample Output

MessageId=0xFFFF SymbolicName=NDAS_MSG_ERR_UNKNOWN
Severity=Error
Facility=Application
Language=ENU
Unknown Application Error
.
Language=KOR
알 수 없는 오류
.

-->
<xsl:template match="message">
MessageId=<xsl:value-of select="@id" />
SymbolicName=<xsl:value-of select="@symbolicName" />
Severity=<xsl:value-of select="@severity" />
Facility=<xsl:value-of select="@facility" />
<xsl:variable name="neu-text" select="text[@lang='NEU']" />
<xsl:for-each select="text">
Language=<xsl:value-of select="@lang" />
<!-- xsl:text is required for CR (Carridge Return) -->
<xsl:text>
</xsl:text>
<xsl:choose>
<xsl:when test="text()='-'"><xsl:value-of select="$neu-text" /></xsl:when>
<xsl:when test="not(text())"><xsl:value-of select="$neu-text" /></xsl:when>
<xsl:otherwise><xsl:value-of select="text()"/></xsl:otherwise>
</xsl:choose>
.
</xsl:for-each>
</xsl:template>

<!--
<xsl:template match="$lang">
Language=<xsl:value-of select="." />
<xsl:text>
</xsl:text>
<xsl:value-of select="text()" />
.
</xsl:template>
-->

</xsl:stylesheet>