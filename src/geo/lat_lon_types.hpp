// Copyright 2010-2014 RethinkDB, all rights reserved.
#ifndef GEO_LAT_LON_TYPES_HPP_
#define GEO_LAT_LON_TYPES_HPP_

#include <utility>
#include <vector>

typedef std::pair<double, double> lat_lon_point_t;
typedef std::vector<lat_lon_point_t> lat_lon_line_t;

#endif  // GEO_LAT_LON_TYPES_HPP_
