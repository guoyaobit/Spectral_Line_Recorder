#pragma once

#include <sdfits.h>
#include <yaml-cpp/yaml.h>
#include <cmath>
#include <cstdio>
#include <stdexcept>

// Shared by local and Recorder writers. Unknown sky coordinates stay NaN.
inline void configure_sdfits_metadata(sdfits &writer, const YAML::Node &meta,
                                     const std::string &object,
                                     bool cal_mode, bool source_on) {
  auto &h = writer.hdr;
  std::snprintf(h.telescope, sizeof(h.telescope), "%s", "NSRT");
  std::snprintf(h.instrument, sizeof(h.instrument), "%s", "7mm");
  std::snprintf(h.cal_mode, sizeof(h.cal_mode), "%s", cal_mode ? "SYNC" : "OFF");
  if (object.size() >= sizeof(writer.data_columns.object))
    throw std::runtime_error("SDFITS OBJECT exceeds 15 characters");
  std::snprintf(writer.data_columns.object, sizeof(writer.data_columns.object),
                "%s", object.empty() ? "UNKNOWN" : object.c_str());
  h.bandwidth = std::abs(h.chan_bw) * h.nchan;
  h.source_on = source_on;
  if (!meta || meta.IsNull()) return;
  if (!meta.IsMap()) throw std::runtime_error("sdfits_metadata must be a mapping");
  if (meta["telescope"]) {
    const auto value = meta["telescope"].as<std::string>();
    if (value.empty() || value.size() >= sizeof(h.telescope))
      throw std::runtime_error("sdfits_metadata.telescope must contain 1-15 characters");
    std::snprintf(h.telescope, sizeof(h.telescope), "%s", value.c_str());
  }
  if (meta["instrument"]) {
    const auto value = meta["instrument"].as<std::string>();
    if (value.empty() || value.size() >= sizeof(h.instrument))
      throw std::runtime_error("sdfits_metadata.instrument must contain 1-15 characters");
    std::snprintf(h.instrument, sizeof(h.instrument), "%s", value.c_str());
  }
  if (meta["site_lon_deg"]) h.site_lon = meta["site_lon_deg"].as<double>();
  if (meta["site_lat_deg"]) h.site_lat = meta["site_lat_deg"].as<double>();
  if (meta["site_height_m"]) h.site_height = meta["site_height_m"].as<double>();
  if (meta["site_lon_deg"] && (!std::isfinite(h.site_lon) || std::abs(h.site_lon) > 180))
    throw std::runtime_error("site_lon_deg must be in [-180,180]");
  if (meta["site_lat_deg"] && (!std::isfinite(h.site_lat) || std::abs(h.site_lat) > 90))
    throw std::runtime_error("site_lat_deg must be in [-90,90]");
  if (meta["site_height_m"] && !std::isfinite(h.site_height))
    throw std::runtime_error("site_height_m must be finite");
  if (meta["ra_deg"]) writer.data_columns.ra = meta["ra_deg"].as<double>();
  if (meta["dec_deg"]) writer.data_columns.dec = meta["dec_deg"].as<double>();
  if (meta["ra_deg"] && (!std::isfinite(writer.data_columns.ra) ||
      writer.data_columns.ra < 0 || writer.data_columns.ra >= 360))
    throw std::runtime_error("sdfits_metadata.ra_deg must be in [0,360)");
  if (meta["dec_deg"] && (!std::isfinite(writer.data_columns.dec) ||
      std::abs(writer.data_columns.dec) > 90))
    throw std::runtime_error("sdfits_metadata.dec_deg must be in [-90,90]");
}
