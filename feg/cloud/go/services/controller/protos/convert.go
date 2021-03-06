/*
Copyright (c) Facebook, Inc. and its affiliates.
All rights reserved.

This source code is licensed under the BSD-style license found in the
LICENSE file in the root directory of this source tree.
*/

package protos

import "magma/feg/cloud/go/protos/mconfig"

// ToMconfig creates new mconfig.DiamServerConfig, copies controller diameter
// server config proto to a managed config proto & returns the new mconfig.DiamServerConfig
func (config *DiamServerConfig) ToMconfig() *mconfig.DiamServerConfig {
	return &mconfig.DiamServerConfig{
		Protocol:     config.GetProtocol(),
		Address:      config.GetAddress(),
		LocalAddress: config.GetLocalAddress(),
		DestRealm:    config.GetDestRealm(),
		DestHost:     config.GetDestHost(),
	}
}

// ToMconfig copies diameter client config controller proto to a managed config proto & returns it
func (config *DiamClientConfig) ToMconfig() *mconfig.DiamClientConfig {
	return &mconfig.DiamClientConfig{
		Protocol:         config.GetProtocol(),
		Address:          config.GetAddress(),
		Retransmits:      config.GetRetransmits(),
		WatchdogInterval: config.GetWatchdogInterval(),
		RetryCount:       config.GetRetryCount(),
		LocalAddress:     config.GetLocalAddress(),
		ProductName:      config.GetProductName(),
		Realm:            config.GetRealm(),
		Host:             config.GetHost(),
		DestRealm:        config.GetDestRealm(),
		DestHost:         config.GetDestHost(),
	}
}

// ToMconfig copies controller subscription profile proto to a a new managed config proto & returns it
func (profile *HSSConfig_SubscriptionProfile) ToMconfig() *mconfig.HSSConfig_SubscriptionProfile {
	return &mconfig.HSSConfig_SubscriptionProfile{
		MaxUlBitRate: profile.GetMaxUlBitRate(),
		MaxDlBitRate: profile.GetMaxDlBitRate(),
	}
}
