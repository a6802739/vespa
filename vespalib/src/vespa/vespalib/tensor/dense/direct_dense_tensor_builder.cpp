// Copyright 2016 Yahoo Inc. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.

#include <vespa/fastos/fastos.h>
#include "direct_dense_tensor_builder.h"

namespace vespalib {
namespace tensor {

using Address = DirectDenseTensorBuilder::Address;
using DimensionsMeta = DirectDenseTensorBuilder::DimensionsMeta;

namespace {

size_t
calculateCellsSize(const DimensionsMeta &dimensionsMeta)
{
    size_t cellsSize = 1;
    for (const auto &dimMeta : dimensionsMeta) {
        cellsSize *= dimMeta.size();
    }
    return cellsSize;
}

size_t
calculateCellAddress(const Address &address, const DimensionsMeta &dimensionsMeta)
{
    assert(address.size() == dimensionsMeta.size());
    size_t result = 0;
    for (size_t i = 0; i < address.size(); ++i) {
        result *= dimensionsMeta[i].size();
        result += address[i];
    }
    return result;
}

}

DirectDenseTensorBuilder::DirectDenseTensorBuilder(const DimensionsMeta &dimensionsMeta)
    : _dimensionsMeta(dimensionsMeta),
      _cells(calculateCellsSize(_dimensionsMeta))
{
}

void
DirectDenseTensorBuilder::insertCell(const Address &address, double cellValue)
{
    size_t cellAddress = calculateCellAddress(address, _dimensionsMeta);
    assert(cellAddress < _cells.size());
    _cells[cellAddress] = cellValue;
}

Tensor::UP
DirectDenseTensorBuilder::build()
{
    return std::make_unique<DenseTensor>(std::move(_dimensionsMeta), std::move(_cells));
}

} // namespace tensor
} // namesapce vespalib
