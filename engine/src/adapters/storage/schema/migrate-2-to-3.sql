-- Version 2 -> 3: sessions gain the retention flag. Existing sessions were
-- recorded before the setting existed and are kept.
ALTER TABLE sessions ADD COLUMN retain INTEGER NOT NULL DEFAULT 1;
