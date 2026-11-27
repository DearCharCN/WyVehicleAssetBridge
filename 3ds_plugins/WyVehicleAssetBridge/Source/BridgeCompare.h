#pragma once
#include <nlohmann/json.hpp>
#include <string>
bool WySameScope(const nlohmann::json& a, const nlohmann::json& b, const std::string& vehicleName);
