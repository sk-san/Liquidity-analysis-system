#pragma once

#include "shadow_book/shadow_book.hpp"

namespace calculation_engine::order_book {

using ShadowBook = abides::shadow::ShadowBook;
using LevelView = abides::shadow::LevelView;
using TopOfBook = abides::shadow::TopOfBook;
using TradeStats = abides::shadow::TradeStats;
using ApplyCode = abides::shadow::ApplyCode;
using ApplyResult = abides::shadow::ApplyResult;

}  // namespace calculation_engine::order_book
