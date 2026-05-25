# DCC08 C++ Encrypt Service

Baseline-compatible C++ implementation for "第八届：高并发内存动态脱敏性能竞速".

## Fixed Protocol And Crypto Constraints

This implementation keeps the competition constraints unchanged:

- `GET /health`
- `POST /encrypt`
- callback payload: `teamCode`, `requestId`, `ip`
- SM4 CBC mode
- IV: `1234567890123456`
- request-provided 16-byte `sm4Key`
- output CSV: UTF-8, no header, no quotes, requested field order, original row order

## Internal Optimization Strategy

- CSV is loaded once into column-oriented storage.
- Mask columns are precomputed after the first successful health check.
- `/encrypt` only parses the request and enqueues a job.
- Jobs are processed by a dispatcher to avoid 100 requests fighting for disk.
- Each job renders row tiles with multiple compute threads, then writes tile buffers in order.
- Files are written as `requestId.csv.tmp`, closed, renamed to `requestId.csv`, then callback is sent.
- SM4 uses an in-process scalar T-table style implementation with uppercase hex output.

The SM4 API is isolated in `src/sm4.*`, so an AVX2/AVX512 batch kernel can replace the scalar block routine later without touching HTTP or output protocol code.

## Build

```bash
make clean
make dcc_encrypt test
```

## Run Locally

```bash
DCC_PORT=18080 \
DCC_DATA_PATH=/Users/april/Documents/dcc_c++/doc/table_data.csv \
DCC_OUTPUT_DIR=/tmp/dcc_cpp_output \
DCC_TEAM_CODE=baseline \
DCC_DISABLE_CALLBACK=1 \
./dcc_encrypt
```

Then:

```bash
curl -s http://127.0.0.1:18080/health
curl -s -X POST http://127.0.0.1:18080/encrypt \
  -H 'Content-Type: application/json' \
  -d '{"requestId":"REQ_LOCAL_1","sm4Key":"2123433411630000","ip":"55.51.53.74","fieldsToEncrypt":["phone","user_code","name"]}'
```

## Competition Environment Variables

- `DCC_TEAM_CODE`: team code used in callback and default output path.
- `DCC_DATA_PATH`: default `/dcc/root/table_data.csv`.
- `DCC_OUTPUT_DIR`: default `/opt/app/dcc/${DCC_TEAM_CODE}/output/`.
- `DCC_CALLBACK_URL`: default `http://dcc08-data-encrypt.paas.cmbchina.cn/callback`.
- `DCC_JOB_WORKERS`: default `4`, number of concurrent background requests.
- `DCC_COMPUTE_THREADS`: default `1`, per-request tile workers. Keep this at `1` when `DCC_JOB_WORKERS` is near CPU count.
- `DCC_QUEUE_COALESCE_MS`: default `0`, optional short background queue batching window for priority scheduling.
- `DCC_TILE_ROWS`: default `100000`.
- `DCC_PORT`: default `8080`.
- `DCC_DISABLE_CALLBACK`: set to `1` only for local testing.

## Submit Command Template

Update `DCC_TEAM_CODE` and paths to your team directory before packaging. Do not set
`DCC_DISABLE_CALLBACK=1` in the competition environment; callback is enabled by
default and sent after each output file is closed and renamed.

```bash
nohup env DCC_TEAM_CODE=teamXXX \
DCC_DATA_PATH=/dcc/root/table_data.csv \
DCC_CALLBACK_URL=http://dcc08-data-encrypt.paas.cmbchina.cn/callback \
DCC_PORT=8080 \
/opt/app/dcc/teamXXX/dcc_encrypt > /opt/app/dcc/teamXXX/dcc.log 2>&1 &
```
