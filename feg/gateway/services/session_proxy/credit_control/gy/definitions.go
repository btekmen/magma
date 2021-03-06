/*
Copyright (c) Facebook, Inc. and its affiliates.
All rights reserved.

This source code is licensed under the BSD-style license found in the
LICENSE file in the root directory of this source tree.
*/

// CreditControl constants and structs to be used in sending/receiving messages
package gy

import (
	"magma/feg/gateway/services/session_proxy/credit_control"
)

const (
	ServiceContextIDDefault = "32251@3gpp.org" // Packet-Switch service context
	ServiceIDDefault        = 0
)

type FinalUnitAction uint8

const (
	Terminate      FinalUnitAction = 0x0
	Redirect       FinalUnitAction = 0x1
	RestrictAccess FinalUnitAction = 0x2
)

type UsedCreditsType int32

const (
	THRESHOLD UsedCreditsType = iota
	QHT
	FINAL                  // FINAL - UE disconnected, flow not in use
	QUOTA_EXHAUSTED        // UE hit credit limit
	VALIDITY_TIMER_EXPIRED // Credit expired
	OTHER_QUOTA_TYPE
	RATING_CONDITION_CHANGE
	FORCED_REAUTHORISATION
	POOL_EXHAUSTED
)

type UsedCredits struct {
	RatingGroup  uint32
	InputOctets  uint64
	OutputOctets uint64
	TotalOctets  uint64
	Type         UsedCreditsType
}

type CreditControlRequest struct {
	SessionID     string
	Type          credit_control.CreditRequestType
	IMSI          string
	RequestNumber uint32
	UeIPV4        string
	SpgwIPV4      string
	Apn           string
	Imei          string
	PlmnID        string
	GcID          string
	UserLocation  []byte
	Msisdn        []byte
	Qos           *QosRequestInfo
	Credits       []*UsedCredits
}

type QosRequestInfo struct {
	ApnAggMaxBitRateUL uint32
	ApnAggMaxBitRateDL uint32
}

type ReceivedCredits struct {
	ResultCode   uint32
	RatingGroup  uint32
	GrantedUnits *credit_control.GrantedServiceUnit
	ValidityTime uint32
	IsFinal      bool
	FinalAction  FinalUnitAction // unused if IsFinal is false
}

type CreditControlAnswer struct {
	ResultCode    uint32
	SessionID     string
	RequestNumber uint32
	Credits       []*ReceivedCredits
}

type FinalUnitIndication struct {
	Action FinalUnitAction `avp:"Final-Unit-Action"`
}

type MSCCDiameterMessage struct {
	ResultCode          uint32                            `avp:"Result-Code"`
	GrantedServiceUnit  credit_control.GrantedServiceUnit `avp:"Granted-Service-Unit"`
	ValidityTime        uint32                            `avp:"Validity-Time"`
	FinalUnitIndication *FinalUnitIndication              `avp:"Final-Unit-Indication"`
	RatingGroup         uint32                            `avp:"Rating-Group"`
}

type CCADiameterMessage struct {
	SessionID     string                 `avp:"Session-Id"`
	RequestNumber uint32                 `avp:"CC-Request-Number"`
	ResultCode    uint32                 `avp:"Result-Code"`
	RequestType   uint32                 `avp:"CC-Request-Type"`
	CreditControl []*MSCCDiameterMessage `avp:"Multiple-Services-Credit-Control"`
}

// ReAuthRequest is a diameter request received from the OCS to initiate a
// credit update
type ReAuthRequest struct {
	SessionID   string  `avp:"Session-Id"`
	RatingGroup *uint32 `avp:"Rating-Group"`
}

// ReAuthAnswer is a diameter answer sent back to the OCS after a credit update
// is initiated
type ReAuthAnswer struct {
	SessionID  string `avp:"Session-Id"`
	ResultCode uint32 `avp:"Result-Code"`
}
