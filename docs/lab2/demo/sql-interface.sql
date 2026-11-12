-- Run in the MySQL client connected to the local SeekDB instance.
SET SESSION enable_file_sql = ON;

-- Named options travel through resolution and the executable plan.
SELECT *
FROM read_csv('/home/wangjiayi/seekdb/docs/lab2/demo/options.csv', delimiter=';', nullstr='NA') AS f
ORDER BY id;

-- Return the schema of selected columns, including aliases and nullability.
DESCRIBE SELECT id, name, amount AS total
FROM read_csv('/home/wangjiayi/seekdb/docs/lab2/demo/sales.csv', header=true, sample_size=-1);

-- Discover regular files in sorted order.
SELECT substring_index(file, '/', -1) AS file
FROM glob('/home/wangjiayi/seekdb/docs/lab2/demo/*.csv');

-- The aggregate executes inside SeekDB.
SELECT COUNT(*) AS rows_read, SUM(amount) AS total
FROM read_csv('/home/wangjiayi/seekdb/docs/lab2/demo/sales.csv');

EXPLAIN SELECT name, amount
FROM read_csv('/home/wangjiayi/seekdb/docs/lab2/demo/sales.csv')
WHERE amount > 8 ORDER BY amount;

-- The test runner replaces this placeholder with a fresh output path.
COPY (SELECT id, name, amount
      FROM read_csv('/home/wangjiayi/seekdb/docs/lab2/demo/sales.csv') ORDER BY id)
TO '__COPY_DEMO_PATH__' (format csv, header, nullstr 'NULL');

SELECT id, name, amount
FROM read_csv('__COPY_DEMO_PATH__', nullstr='NULL') ORDER BY id;
