DROP TABLE IF EXISTS merge_a;
DROP TABLE IF EXISTS merge_b;
DROP TABLE IF EXISTS merge_ab;

CREATE TABLE merge_a (x UInt8) ENGINE = StripeLog;
CREATE TABLE merge_b (x UInt8) ENGINE = StripeLog;
CREATE TABLE merge_ab (x UInt8) ENGINE = Merge(currentDatabase(), '^merge_[ab]$');

SELECT * FROM system.columns WHERE database = currentDatabase() AND table = 'merge_ab';

DROP TABLE merge_a;
DROP TABLE merge_b;
DROP TABLE merge_ab;
