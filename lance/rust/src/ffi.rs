use std::ffi::{c_char, c_void, CStr, CString};
use std::pin::Pin;
use std::ptr;
use std::sync::Arc;

use arrow::array::{Array, RecordBatch, StructArray};
use arrow::datatypes::DataType;
use arrow::ffi::{FFI_ArrowArray, FFI_ArrowSchema};
use arrow_schema::{Field as ArrowField, Schema as ArrowSchema};
use futures::StreamExt;
use lance::dataset::builder::DatasetBuilder;
use lance::dataset::scanner::DatasetRecordBatchStream;
use lance::Dataset;
use lance_core::datatypes::{LogicalType as LanceLogicalType, Schema as LanceSchema};
use tokio::runtime::Handle;

use crate::error::{clear_last_error, set_last_error, ErrorCode};
use crate::runtime;

const VERSION_METADATA_KEY: &str = "lance_version";

/// Max rows per exported batch slice. Matches Ladybug's default
/// `DEFAULT_VECTOR_CAPACITY` (2^11); the C++ side converts each slice into a
/// fresh engine chunk, so slices must never exceed it.
const BATCH_SLICE_ROWS: usize = 2048;

pub(crate) struct DatasetHandle {
    dataset: Arc<Dataset>,
}

pub(crate) struct StreamHandle {
    handle: Handle,
    stream: Pin<Box<DatasetRecordBatchStream>>,
    pending: Option<RecordBatch>,
    pending_pos: usize,
}

struct FfiError {
    code: ErrorCode,
    message: String,
}

impl FfiError {
    fn new(code: ErrorCode, message: String) -> Self {
        Self { code, message }
    }
}

type FfiResult<T> = Result<T, FfiError>;

fn dataset_handle(dataset: *mut c_void) -> FfiResult<&'static DatasetHandle> {
    if dataset.is_null() {
        return Err(FfiError::new(
            ErrorCode::InvalidArgument,
            "dataset is null".to_string(),
        ));
    }
    Ok(unsafe { &*(dataset as *const DatasetHandle) })
}

fn stream_handle_mut(stream: *mut c_void) -> FfiResult<&'static mut StreamHandle> {
    if stream.is_null() {
        return Err(FfiError::new(
            ErrorCode::InvalidArgument,
            "stream is null".to_string(),
        ));
    }
    Ok(unsafe { &mut *(stream as *mut StreamHandle) })
}

fn batch_handle(batch: *mut c_void) -> FfiResult<&'static RecordBatch> {
    if batch.is_null() {
        return Err(FfiError::new(
            ErrorCode::InvalidArgument,
            "batch is null".to_string(),
        ));
    }
    Ok(unsafe { &*(batch as *const RecordBatch) })
}

fn cstr_to_str<'a>(ptr: *const c_char, name: &str) -> FfiResult<&'a str> {
    if ptr.is_null() {
        return Err(FfiError::new(
            ErrorCode::InvalidArgument,
            format!("{name} is null"),
        ));
    }
    unsafe { CStr::from_ptr(ptr) }
        .to_str()
        .map_err(|err| FfiError::new(ErrorCode::Utf8, format!("{name} utf8: {err}")))
}

fn cstr_array(ptr: *const *const c_char, len: usize, name: &str) -> FfiResult<Vec<String>> {
    if len == 0 {
        return Ok(Vec::new());
    }
    if ptr.is_null() {
        return Err(FfiError::new(
            ErrorCode::InvalidArgument,
            format!("{name} is null with non-zero length"),
        ));
    }
    let mut out = Vec::with_capacity(len);
    for i in 0..len {
        let item = unsafe { *ptr.add(i) };
        out.push(cstr_to_str(item, name)?.to_string());
    }
    Ok(out)
}

fn to_c_string(value: String) -> CString {
    match CString::new(value) {
        Ok(v) => v,
        Err(_) => CString::new("invalid string").unwrap(),
    }
}

fn reject(lt: &LanceLogicalType, reason: &str) -> FfiError {
    FfiError::new(
        ErrorCode::SchemaExport,
        format!("unsupported lance column type '{lt}': {reason}"),
    )
}

/// Map one Lance schema field to its Arrow field. Leaf types convert through
/// Lance's own `TryFrom<&LogicalType>`; structural types recurse into
/// children. Anything else fails fast so values can never be silently
/// mistyped downstream.
fn lance_field_to_arrow(field: &lance_core::datatypes::Field) -> FfiResult<ArrowField> {
    let text = field.logical_type.to_string();
    let data_type = if text == "struct" {
        let children = field
            .children
            .iter()
            .map(lance_field_to_arrow)
            .collect::<FfiResult<Vec<_>>>()?;
        DataType::Struct(children.into())
    } else if text == "list"
        || text == "list.struct"
        || text == "large_list"
        || text == "large_list.struct"
    {
        let element = field.children.first().ok_or_else(|| {
            FfiError::new(
                ErrorCode::SchemaExport,
                "list column has no element".to_string(),
            )
        })?;
        let element = lance_field_to_arrow(element)?;
        if text.starts_with("large_list") {
            DataType::LargeList(Arc::new(element))
        } else {
            DataType::List(Arc::new(element))
        }
    } else if text.starts_with("fixed_size_list:") {
        let element = field.children.first().ok_or_else(|| {
            FfiError::new(
                ErrorCode::SchemaExport,
                "fixed-size list column has no element".to_string(),
            )
        })?;
        let element = lance_field_to_arrow(element)?;
        let width: i32 = text
            .rsplit(':')
            .next()
            .and_then(|w| w.parse().ok())
            .ok_or_else(|| {
                FfiError::new(
                    ErrorCode::SchemaExport,
                    format!("cannot parse fixed-size list width from '{text}'"),
                )
            })?;
        DataType::FixedSizeList(Arc::new(element), width)
    } else if text == "map" {
        return Err(reject(
            &field.logical_type,
            "map columns are not supported in v1; flatten them into top-level columns",
        ));
    } else if text.starts_with("timestamp:") {
        // Timestamps with a zone would be silently mistyped downstream, and
        // durations/time32/time64 have no exact counterpart either.
        let tz_naive = text == "timestamp:s:-"
            || text == "timestamp:ms:-"
            || text == "timestamp:us:-"
            || text == "timestamp:ns:-";
        if !tz_naive {
            return Err(reject(
                &field.logical_type,
                "only timezone-naive timestamps are supported",
            ));
        }
        DataType::try_from(&field.logical_type).map_err(|err| {
            FfiError::new(
                ErrorCode::SchemaExport,
                format!("timestamp conversion: {err}"),
            )
        })?
    } else if text.starts_with("duration:")
        || text.starts_with("time32:")
        || text.starts_with("time64:")
    {
        return Err(reject(
            &field.logical_type,
            "only date/timestamp/interval temporal types are supported",
        ));
    } else {
        DataType::try_from(&field.logical_type).map_err(|err| {
            FfiError::new(ErrorCode::SchemaExport, format!("type conversion: {err}"))
        })?
    };
    Ok(ArrowField::new(&field.name, data_type, field.nullable))
}

fn lance_schema_to_arrow_schema(schema: &LanceSchema) -> FfiResult<ArrowSchema> {
    let fields = schema
        .fields
        .iter()
        .map(lance_field_to_arrow)
        .collect::<FfiResult<Vec<_>>>()?;
    Ok(ArrowSchema::new(fields))
}

#[no_mangle]
pub unsafe extern "C" fn lance_open_dataset(path: *const c_char) -> *mut c_void {
    match open_dataset_inner(path) {
        Ok(handle) => {
            clear_last_error();
            Box::into_raw(Box::new(handle)) as *mut c_void
        }
        Err(err) => {
            set_last_error(err.code, err.message);
            ptr::null_mut()
        }
    }
}

fn open_dataset_inner(path: *const c_char) -> FfiResult<DatasetHandle> {
    let path_str = cstr_to_str(path, "path")?;
    // Opening pins the dataset handle to the latest committed version: every
    // scan through the handle reads one stable snapshot.
    let dataset = match runtime::block_on(DatasetBuilder::from_uri(path_str).load()) {
        Ok(Ok(ds)) => Arc::new(ds),
        Ok(Err(err)) => {
            return Err(FfiError::new(
                ErrorCode::DatasetOpen,
                format!("dataset open '{path_str}': {err}"),
            ))
        }
        Err(err) => return Err(FfiError::new(ErrorCode::Runtime, format!("runtime: {err}"))),
    };
    Ok(DatasetHandle { dataset })
}

#[no_mangle]
pub unsafe extern "C" fn lance_close_dataset(dataset: *mut c_void) {
    if !dataset.is_null() {
        unsafe {
            let _ = Box::from_raw(dataset as *mut DatasetHandle);
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn lance_dataset_count_rows(dataset: *mut c_void) -> i64 {
    match dataset_count_rows_inner(dataset) {
        Ok(v) => {
            clear_last_error();
            v
        }
        Err(err) => {
            set_last_error(err.code, err.message);
            -1
        }
    }
}

fn dataset_count_rows_inner(dataset: *mut c_void) -> FfiResult<i64> {
    let handle = dataset_handle(dataset)?;
    let rows = match runtime::block_on(handle.dataset.count_rows(None)) {
        Ok(Ok(rows)) => rows,
        Ok(Err(err)) => {
            return Err(FfiError::new(
                ErrorCode::DatasetCountRows,
                format!("dataset count_rows: {err}"),
            ))
        }
        Err(err) => return Err(FfiError::new(ErrorCode::Runtime, format!("runtime: {err}"))),
    };
    i64::try_from(rows).map_err(|_| {
        FfiError::new(
            ErrorCode::DatasetCountRows,
            "row count overflow".to_string(),
        )
    })
}

#[no_mangle]
pub unsafe extern "C" fn lance_dataset_version(dataset: *mut c_void, out_version: *mut u64) -> i32 {
    if out_version.is_null() {
        set_last_error(ErrorCode::InvalidArgument, "out_version must not be null");
        return -1;
    }
    match dataset_handle(dataset) {
        Ok(handle) => {
            clear_last_error();
            unsafe {
                *out_version = handle.dataset.manifest.version;
            }
            0
        }
        Err(err) => {
            set_last_error(err.code, err.message);
            -1
        }
    }
}

/// Export the dataset's Arrow schema (all columns, in dataset order).
#[no_mangle]
pub unsafe extern "C" fn lance_schema_to_arrow(
    dataset: *mut c_void,
    out_schema: *mut FFI_ArrowSchema,
) -> i32 {
    match schema_to_arrow_inner(dataset, out_schema) {
        Ok(()) => {
            clear_last_error();
            0
        }
        Err(err) => {
            set_last_error(err.code, err.message);
            -1
        }
    }
}

fn schema_to_arrow_inner(dataset: *mut c_void, out_schema: *mut FFI_ArrowSchema) -> FfiResult<()> {
    if out_schema.is_null() {
        return Err(FfiError::new(
            ErrorCode::InvalidArgument,
            "out_schema is null".to_string(),
        ));
    }
    let handle = dataset_handle(dataset)?;
    let arrow_schema = lance_schema_to_arrow_schema(handle.dataset.schema())?;
    let ffi_schema = FFI_ArrowSchema::try_from(&DataType::Struct(arrow_schema.fields().clone()))
        .map_err(|err| FfiError::new(ErrorCode::SchemaExport, format!("schema export: {err}")))?;
    unsafe {
        std::ptr::write_unaligned(out_schema, ffi_schema);
    }
    Ok(())
}

/// Return the dataset's `lance_version` schema-metadata value, or an empty
/// string when the marker is absent.
#[no_mangle]
pub unsafe extern "C" fn lance_schema_version_string(dataset: *mut c_void) -> *const c_char {
    match schema_version_string_inner(dataset) {
        Ok(value) => {
            clear_last_error();
            to_c_string(value).into_raw() as *const c_char
        }
        Err(err) => {
            set_last_error(err.code, err.message);
            ptr::null()
        }
    }
}

fn schema_version_string_inner(dataset: *mut c_void) -> FfiResult<String> {
    let handle = dataset_handle(dataset)?;
    Ok(handle
        .dataset
        .schema()
        .metadata
        .get(VERSION_METADATA_KEY)
        .cloned()
        .unwrap_or_default())
}

#[no_mangle]
pub unsafe extern "C" fn lance_scan_open(
    dataset: *mut c_void,
    columns: *const *const c_char,
    columns_len: usize,
    limit: i64,
    offset: u64,
) -> *mut c_void {
    match scan_open_inner(dataset, columns, columns_len, limit, offset) {
        Ok(stream) => {
            clear_last_error();
            Box::into_raw(Box::new(stream)) as *mut c_void
        }
        Err(err) => {
            set_last_error(err.code, err.message);
            ptr::null_mut()
        }
    }
}

fn scan_open_inner(
    dataset: *mut c_void,
    columns: *const *const c_char,
    columns_len: usize,
    limit: i64,
    offset: u64,
) -> FfiResult<StreamHandle> {
    let handle = dataset_handle(dataset)?;
    if limit < -1 {
        return Err(FfiError::new(
            ErrorCode::ScanCreate,
            "limit must be >= -1".to_string(),
        ));
    }
    let projection = cstr_array(columns, columns_len, "columns")?;

    let mut scan = handle.dataset.scan();
    if !projection.is_empty() {
        scan.project(&projection)
            .map_err(|err| FfiError::new(ErrorCode::ScanCreate, format!("scan project: {err}")))?;
    }
    scan.blob_handling(lance_core::datatypes::BlobHandling::AllBinary);
    if limit != -1 || offset != 0 {
        let limit_opt = if limit == -1 { None } else { Some(limit) };
        let offset_opt =
            if offset == 0 {
                None
            } else {
                Some(i64::try_from(offset).map_err(|_| {
                    FfiError::new(ErrorCode::ScanCreate, "offset overflow".to_string())
                })?)
            };
        scan.limit(limit_opt, offset_opt)
            .map_err(|err| FfiError::new(ErrorCode::ScanCreate, format!("scan limit: {err}")))?;
    }
    // In-order scans: CSR range reads map batch positions back to source rows,
    // which requires dataset order.
    scan.scan_in_order(true);

    let rt_handle = runtime::handle()
        .map_err(|err| FfiError::new(ErrorCode::Runtime, format!("runtime: {err}")))?;
    let stream = rt_handle
        .block_on(async { scan.try_into_stream().await })
        .map_err(|err| FfiError::new(ErrorCode::ScanCreate, format!("stream create: {err}")))?;
    Ok(StreamHandle {
        handle: rt_handle,
        stream: Box::pin(stream),
        pending: None,
        pending_pos: 0,
    })
}

/// Fetch the next batch slice (at most BATCH_SLICE_ROWS rows).
/// Returns 0 with a batch, 1 when exhausted, -1 on error.
#[no_mangle]
pub unsafe extern "C" fn lance_scan_next(stream: *mut c_void, out_batch: *mut *mut c_void) -> i32 {
    match scan_next_inner(stream, out_batch) {
        Ok(v) => {
            clear_last_error();
            v
        }
        Err(err) => {
            set_last_error(err.code, err.message);
            -1
        }
    }
}

fn scan_next_inner(stream: *mut c_void, out_batch: *mut *mut c_void) -> FfiResult<i32> {
    if out_batch.is_null() {
        return Err(FfiError::new(
            ErrorCode::ScanNext,
            "out_batch is null".to_string(),
        ));
    }
    unsafe {
        ptr::write_unaligned(out_batch, ptr::null_mut());
    }
    let handle = stream_handle_mut(stream)?;

    loop {
        if let Some(batch) = handle.pending.as_ref() {
            let remaining = batch.num_rows().saturating_sub(handle.pending_pos);
            if remaining > 0 {
                let len = remaining.min(BATCH_SLICE_ROWS);
                let slice = batch.slice(handle.pending_pos, len);
                handle.pending_pos += len;
                if handle.pending_pos >= batch.num_rows() {
                    handle.pending = None;
                    handle.pending_pos = 0;
                }
                let out = Box::into_raw(Box::new(slice)) as *mut c_void;
                unsafe {
                    ptr::write_unaligned(out_batch, out);
                }
                return Ok(0);
            }
            handle.pending = None;
            handle.pending_pos = 0;
        }
        let next = handle.handle.block_on(async { handle.stream.next().await });
        match next {
            Some(Ok(batch)) => {
                if batch.num_rows() == 0 {
                    continue;
                }
                handle.pending = Some(batch);
                handle.pending_pos = 0;
            }
            Some(Err(err)) => {
                return Err(FfiError::new(
                    ErrorCode::ScanNext,
                    format!("stream next: {err}"),
                ))
            }
            None => return Ok(1),
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn lance_close_scan(stream: *mut c_void) {
    if !stream.is_null() {
        unsafe {
            let _ = Box::from_raw(stream as *mut StreamHandle);
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn lance_batch_to_arrow(
    batch: *mut c_void,
    out_array: *mut FFI_ArrowArray,
    out_schema: *mut FFI_ArrowSchema,
) -> i32 {
    match batch_to_arrow_inner(batch, out_array, out_schema) {
        Ok(()) => {
            clear_last_error();
            0
        }
        Err(err) => {
            set_last_error(err.code, err.message);
            -1
        }
    }
}

fn batch_to_arrow_inner(
    batch: *mut c_void,
    out_array: *mut FFI_ArrowArray,
    out_schema: *mut FFI_ArrowSchema,
) -> FfiResult<()> {
    if out_array.is_null() || out_schema.is_null() {
        return Err(FfiError::new(
            ErrorCode::BatchExport,
            "out_array/out_schema is null".to_string(),
        ));
    }
    let batch = batch_handle(batch)?;
    let struct_array = StructArray::from(batch.clone());
    let data = struct_array.to_data();
    let array = FFI_ArrowArray::new(&data);
    let schema = FFI_ArrowSchema::try_from(data.data_type())
        .map_err(|err| FfiError::new(ErrorCode::BatchExport, format!("batch export: {err}")))?;
    unsafe {
        std::ptr::write_unaligned(out_array, array);
        std::ptr::write_unaligned(out_schema, schema);
    }
    Ok(())
}

#[no_mangle]
pub unsafe extern "C" fn lance_free_batch(batch: *mut c_void) {
    if !batch.is_null() {
        unsafe {
            let _ = Box::from_raw(batch as *mut RecordBatch);
        }
    }
}
