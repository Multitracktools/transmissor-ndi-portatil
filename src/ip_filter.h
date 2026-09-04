#pragma once

#include <string>

bool installReceiverIpFilter(const std::string& allowedIpv4, std::wstring& error);
void removeReceiverIpFilter();
bool receiverIpFilterActive();
