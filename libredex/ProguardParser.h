// Copyright 2004-present Facebook. All Rights Reserved.

//  ProguardParser.h
//  pgparser
//
//  Created by Satnam Singh on 4/20/16.
//  Copyright © 2016 Satnam Singh. All rights reserved.
//

#pragma once

#include <memory>
#include <set>
#include <vector>

#include "ProguardConfiguration.h"
#include "ProguardLexer.h"

namespace redex {
namespace proguard_parser {

void parse_file(const std::string filename, ProguardConfiguration* pg_config);
void parse(istream& config, ProguardConfiguration* pg_config);

} // namespace proguard_parser
} // namespace redex
