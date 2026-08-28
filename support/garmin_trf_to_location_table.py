#!/usr/bin/env python3
"""Convert a Garmin TRF file to nrsc5's UTF-8 location-table TSV format."""

import argparse
import mmap
import os
import tempfile
from dataclasses import dataclass


def uint_le(data, offset, size):
    raw = data[offset:offset + size]
    if len(raw) != size:
        raise ValueError("truncated Garmin TRF file")
    return int.from_bytes(raw, "little")


def int_width(maximum):
    if maximum < 0x100:
        return 1
    if maximum < 0x10000:
        return 2
    if maximum < 0x1000000:
        return 3
    return 4


def signed(value, bits):
    if value & (1 << (bits - 1)):
        return value - (1 << bits)
    return value


class BitReader:
    def __init__(self, data, bit_offset=0, bit_limit=None):
        self.data = data
        self.bit_offset = bit_offset
        self.bit_limit = len(data) * 8 if bit_limit is None else bit_limit

    def read(self, count):
        if self.bit_offset + count > self.bit_limit:
            raise ValueError("truncated Garmin TRF bit field")
        value = 0
        for index in range(count):
            position = self.bit_offset + index
            value |= ((self.data[position // 8] >> (position % 8)) & 1) << index
        self.bit_offset += count
        return value

    def align(self):
        self.bit_offset = (self.bit_offset + 7) & ~7


@dataclass
class Range:
    start: int
    stride: int


@dataclass
class Descriptor:
    country: int
    ltn: int
    delta_bits: int
    bounds: list
    ranges: list
    link_bytes: int
    detail_offsets: list


class TrfFile:
    def __init__(self, data):
        self.data = data
        if len(data) < 0x3B or data[2:12] != b"GARMIN TRF":
            raise ValueError("not a Garmin TRF file")
        self.version = uint_le(data, 0x0C, 2)
        if self.version != 1:
            raise ValueError(f"unsupported Garmin TRF version {self.version}")

        self.coord_bits = data[0x15]
        self.coord_base = uint_le(data, 0x17, 4)
        self.value_limit = uint_le(data, 0x1B, 4)
        self.names_offset = uint_le(data, 0x1F, 4)
        self.names_size = uint_le(data, 0x23, 4)
        self.detail_offset = uint_le(data, 0x29, 4)
        self.detail_size = uint_le(data, 0x2D, 4)
        descriptor_offset = uint_le(data, 0x31, 4)
        descriptor_size = uint_le(data, 0x35, 2)
        descriptor_bytes = uint_le(data, 0x37, 4)
        if not 0 < self.coord_bits < 32 or descriptor_size == 0:
            raise ValueError("invalid Garmin TRF header")
        if descriptor_bytes % descriptor_size:
            raise ValueError("invalid Garmin TRF descriptor section")
        self._check_range(self.coord_base, self.value_limit, "coordinate")
        self._check_range(self.names_offset, self.names_size, "name")
        self._check_range(self.detail_offset, self.detail_size, "detail")
        self._check_range(descriptor_offset, descriptor_bytes, "descriptor")
        self.descriptors = [
            self._descriptor(descriptor_offset + index * descriptor_size, descriptor_size)
            for index in range(descriptor_bytes // descriptor_size)
        ]
        if not self.descriptors:
            raise ValueError("empty Garmin TRF descriptor section")
        keys = {(descriptor.country, descriptor.ltn) for descriptor in self.descriptors}
        if len(keys) != len(self.descriptors):
            raise ValueError("duplicate Garmin TRF location table")

    def _check_range(self, offset, size, label):
        if offset > len(self.data) or size > len(self.data) - offset:
            raise ValueError(f"invalid Garmin TRF {label} section")

    def _descriptor(self, offset, size):
        raw = self.data[offset:offset + size]
        if len(raw) != size:
            raise ValueError("truncated Garmin TRF descriptor")
        packed = uint_le(raw, 0, 3)
        reader = BitReader(raw, 24)
        bounds = [
            signed(reader.read(self.coord_bits), self.coord_bits) << (32 - self.coord_bits)
            for _ in range(4)
        ]
        reader.align()
        position = reader.bit_offset // 8
        value_bytes = int_width(self.value_limit)
        ranges = []
        for _ in range(3):
            ranges.append(Range(uint_le(raw, position, value_bytes),
                                uint_le(raw, position + value_bytes, 2)))
            position += value_bytes + 2
        link_bytes = (uint_le(raw, position, 1) & 3) + 1
        position += 1
        detail_bytes = int_width(self.detail_size)
        detail_offsets = []
        for _ in range(3):
            detail_offsets.append(uint_le(raw, position, detail_bytes))
            position += detail_bytes
        return Descriptor((packed >> 14) & 0x0F, (packed >> 8) & 0x3F,
                          ((packed >> 18) & 0x1F) + 1, bounds, ranges,
                          link_bytes, detail_offsets)

    def name(self, offset):
        if offset >= self.names_size:
            raise ValueError("invalid Garmin TRF name")
        start = self.names_offset + offset
        end = self.data.find(b"\0", start, self.names_offset + self.names_size)
        if end < 0:
            raise ValueError("invalid Garmin TRF name")
        return self.data[start:end].decode("cp1252", errors="replace")

    def points(self, descriptor_index):
        descriptor = self.descriptors[descriptor_index]
        point_range, second_range, third_range = descriptor.ranges
        if not point_range.stride or not second_range.stride or not third_range.stride:
            raise ValueError("invalid Garmin TRF record stride")
        third_end = (self.descriptors[descriptor_index + 1].ranges[0].start
                     if descriptor_index + 1 < len(self.descriptors) else self.value_limit)
        spans = (second_range.start - point_range.start,
                 third_range.start - second_range.start,
                 third_end - third_range.start)
        strides = (point_range.stride, second_range.stride, third_range.stride)
        if (point_range.start > second_range.start
                or second_range.start > third_range.start
                or third_range.start > third_end
                or any(span % stride for span, stride in zip(spans, strides))):
            raise ValueError("invalid Garmin TRF record ranges")
        point_count, second_count, third_count = (
            span // stride for span, stride in zip(spans, strides))
        field_widths = [int_width(third_count), int_width(second_count), int_width(point_count)]

        records = []
        record_start = self.coord_base + point_range.start
        self._check_range(record_start, point_count * point_range.stride, "point")
        previous_location = -1
        for index in range(point_count):
            start = record_start + index * point_range.stride
            raw = self.data[start:start + point_range.stride]
            if len(raw) != point_range.stride:
                raise ValueError("truncated Garmin TRF point record")
            coordinates = BitReader(raw, (2 + descriptor.link_bytes) * 8)
            latitude_delta = signed(coordinates.read(descriptor.delta_bits), descriptor.delta_bits)
            longitude_delta = signed(coordinates.read(descriptor.delta_bits), descriptor.delta_bits)
            shift = 32 - self.coord_bits
            latitude = ((descriptor.bounds[0] + descriptor.bounds[2]) // 2
                        + (latitude_delta << shift)) * 180.0 / (1 << 31)
            longitude = ((descriptor.bounds[1] + descriptor.bounds[3]) // 2
                         + (longitude_delta << shift)) * 180.0 / (1 << 31)
            location = uint_le(raw, 0, 2)
            if location <= previous_location:
                raise ValueError("unordered Garmin TRF point locations")
            previous_location = location
            records.append({
                "location": location,
                "detail": uint_le(raw, 2, descriptor.link_bytes),
                "positive": 0,
                "negative": 0,
                "latitude": latitude,
                "longitude": longitude,
                "name": "",
            })

        name_bits = self.names_size.bit_length()
        detail_end = self.detail_offset + self.detail_size
        for record in records:
            position = (self.detail_offset + descriptor.detail_offsets[0]
                        + record["detail"])
            if (descriptor.detail_offsets[0] > self.detail_size
                    or record["detail"] > self.detail_size - descriptor.detail_offsets[0]
                    or position >= detail_end):
                raise ValueError("invalid Garmin TRF point detail")
            flags = uint_le(self.data, position, 1)
            position += 1
            if flags & 0x04:
                position += field_widths[0]
            if flags & 0x08:
                position += field_widths[1]
            if position > detail_end:
                raise ValueError("invalid Garmin TRF point detail")
            for flag, key in ((0x10, "positive"), (0x20, "negative")):
                if flags & flag:
                    if position + field_widths[2] > detail_end:
                        raise ValueError("invalid Garmin TRF point link")
                    index = uint_le(self.data, position, field_widths[2])
                    position += field_widths[2]
                    if index >= len(records):
                        raise ValueError("invalid Garmin TRF point link")
                    record[key] = records[index]["location"]
            if flags & 0x01:
                reader = BitReader(self.data, position * 8, detail_end * 8)
                language_bits = reader.read(2) * 2
                candidates = []
                for _ in range(64):
                    language = reader.read(language_bits) if language_bits else 0
                    candidates.append((language, reader.read(name_bits)))
                    if reader.read(1):
                        break
                else:
                    raise ValueError("invalid Garmin TRF name list")
                name_offset = next((offset for language, offset in candidates if language == 0),
                                   candidates[0][1])
                record["name"] = self.name(name_offset)
        return records


def sanitize_name(name):
    return name.replace("\t", " ").replace("\r", " ").replace("\n", " ")


def convert(input_path, output_path):
    if os.path.abspath(input_path) == os.path.abspath(output_path):
        raise ValueError("input and output paths must differ")
    output_dir = os.path.dirname(os.path.abspath(output_path))
    temporary_path = None
    with open(input_path, "rb") as source:
        data = mmap.mmap(source.fileno(), 0, access=mmap.ACCESS_READ)
        try:
            trf = TrfFile(data)
            with tempfile.NamedTemporaryFile("w", encoding="utf-8", newline="",
                                             dir=output_dir, delete=False) as output:
                temporary_path = output.name
                output.write("country\tltn\tlocation\tpositive\tnegative\tlatitude\tlongitude\tname\n")
                for descriptor_index, descriptor in enumerate(trf.descriptors):
                    for point in trf.points(descriptor_index):
                        output.write(
                            f"{descriptor.country}\t{descriptor.ltn}\t{point['location']}\t"
                            f"{point['positive']}\t{point['negative']}\t"
                            f"{point['latitude']!r}\t{point['longitude']!r}\t"
                            f"{sanitize_name(point['name'])}\n")
            os.replace(temporary_path, output_path)
            temporary_path = None
        finally:
            data.close()
            if temporary_path is not None:
                try:
                    os.unlink(temporary_path)
                except FileNotFoundError:
                    pass


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", help="version-1 Garmin TRF file")
    parser.add_argument("output", help="output location-table TSV file")
    args = parser.parse_args()
    try:
        convert(args.input, args.output)
    except (OSError, ValueError, IndexError) as error:
        parser.error(str(error))


if __name__ == "__main__":
    main()
