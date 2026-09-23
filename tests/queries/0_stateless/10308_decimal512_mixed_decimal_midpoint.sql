SELECT
    toTypeName(avg2(CAST(1.0 AS Decimal(76, 65)), CAST(1 AS Int64))) AS avg2_type,
    avg2(CAST(1.0 AS Decimal(76, 65)), CAST(1 AS Int64)) AS avg2_value,
    toTypeName(midpoint(CAST(1.0 AS Decimal(76, 65)), CAST(1 AS Int64))) AS midpoint_type,
    midpoint(CAST(1.0 AS Decimal(76, 65)), CAST(1 AS Int64)) AS midpoint_value;
