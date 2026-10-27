//! Regenerates the Lance test fixtures under `test/lance_data/`.
//!
//! Content mirrors `dataset/ice-disk-test` (the icebug-disk fixtures)
//! row-for-row; every dataset is additionally stamped with
//! `lance_version = v1` schema metadata (plus one `v999` dataset for the
//! version-mismatch error test). Run via
//! `extension/lance/scripts/gen_test_data.sh`.
use std::collections::HashMap;
use std::sync::Arc;

use arrow_array::{Int32Array, Int64Array, RecordBatch, RecordBatchIterator, StringArray};
use arrow_schema::{DataType, Field, Schema};
use lance::dataset::{WriteMode, WriteParams};
use lance::Dataset;

fn schema_with_version(fields: Vec<Field>, version: &str) -> Arc<Schema> {
    Arc::new(Schema::new_with_metadata(
        fields,
        HashMap::from([("lance_version".to_string(), version.to_string())]),
    ))
}

async fn write_dataset(out_dir: &str, name: &str, schema: Arc<Schema>, batch: RecordBatch) {
    let uri = format!("{out_dir}/{name}");
    let reader = RecordBatchIterator::new(vec![Ok(batch)].into_iter(), schema);
    let params = WriteParams {
        mode: WriteMode::Overwrite,
        ..Default::default()
    };
    Dataset::write(reader, &uri, Some(params))
        .await
        .unwrap_or_else(|err| panic!("failed to write {uri}: {err}"));
    println!("wrote {uri}");
}

fn int32_col(values: Vec<i32>) -> Arc<dyn arrow_array::Array> {
    Arc::new(Int32Array::from(values))
}

fn int64_col(values: Vec<i64>) -> Arc<dyn arrow_array::Array> {
    Arc::new(Int64Array::from(values))
}

fn str_col(values: Vec<&str>) -> Arc<dyn arrow_array::Array> {
    Arc::new(StringArray::from(values))
}

#[tokio::main]
async fn main() {
    let out_dir = std::env::args()
        .nth(1)
        .expect("usage: gen_fixtures <out_dir>");
    std::fs::create_dir_all(&out_dir).unwrap();

    // nodes_user(id INT32, name VARCHAR, age BIGINT)
    let schema = schema_with_version(
        vec![
            Field::new("id", DataType::Int32, false),
            Field::new("name", DataType::Utf8, false),
            Field::new("age", DataType::Int64, false),
        ],
        "v1",
    );
    let batch = RecordBatch::try_new(
        schema.clone(),
        vec![
            int32_col(vec![75, 100, 250, 300]),
            str_col(vec!["Noura", "Adam", "Karissa", "Zhang"]),
            int64_col(vec![25, 30, 40, 50]),
        ],
    )
    .unwrap();
    write_dataset(&out_dir, "nodes_user.lance", schema, batch).await;

    // nodes_city(id INT32, name VARCHAR, population BIGINT)
    let schema = schema_with_version(
        vec![
            Field::new("id", DataType::Int32, false),
            Field::new("name", DataType::Utf8, false),
            Field::new("population", DataType::Int64, false),
        ],
        "v1",
    );
    let batch = RecordBatch::try_new(
        schema.clone(),
        vec![
            int32_col(vec![500, 600, 700]),
            str_col(vec!["Guelph", "Kitchener", "Waterloo"]),
            int64_col(vec![75000, 200000, 150000]),
        ],
    )
    .unwrap();
    write_dataset(&out_dir, "nodes_city.lance", schema, batch).await;

    // indices_follows(target BIGINT, since INT32), CSR-sorted by source.
    let schema = schema_with_version(
        vec![
            Field::new("target", DataType::Int64, false),
            Field::new("since", DataType::Int32, false),
        ],
        "v1",
    );
    let batch = RecordBatch::try_new(
        schema.clone(),
        vec![
            int64_col(vec![1, 1, 2, 3, 1, 3, 0]),
            int32_col(vec![2023, 2023, 2020, 2020, 2022, 2021, 2022]),
        ],
    )
    .unwrap();
    write_dataset(&out_dir, "indices_follows.lance", schema, batch).await;

    // indptr_follows(ptr BIGINT)
    let schema = schema_with_version(vec![Field::new("ptr", DataType::Int64, false)], "v1");
    let batch = RecordBatch::try_new(schema.clone(), vec![int64_col(vec![0, 1, 4, 6, 7])]).unwrap();
    write_dataset(&out_dir, "indptr_follows.lance", schema, batch).await;

    // indices_livesin(target BIGINT), no edge properties.
    let schema = schema_with_version(vec![Field::new("target", DataType::Int64, false)], "v1");
    let batch = RecordBatch::try_new(schema.clone(), vec![int64_col(vec![0, 2, 2, 1])]).unwrap();
    write_dataset(&out_dir, "indices_livesin.lance", schema, batch).await;

    // indptr_livesin(ptr BIGINT)
    let schema = schema_with_version(vec![Field::new("ptr", DataType::Int64, false)], "v1");
    let batch = RecordBatch::try_new(schema.clone(), vec![int64_col(vec![0, 1, 2, 3, 4])]).unwrap();
    write_dataset(&out_dir, "indptr_livesin.lance", schema, batch).await;

    // rels_follows_flat(src BIGINT, dst BIGINT, since INT32)
    let schema = schema_with_version(
        vec![
            Field::new("src", DataType::Int64, false),
            Field::new("dst", DataType::Int64, false),
            Field::new("since", DataType::Int32, false),
        ],
        "v1",
    );
    let batch = RecordBatch::try_new(
        schema.clone(),
        vec![
            int64_col(vec![0, 1, 1, 1, 2, 2, 3]),
            int64_col(vec![1, 1, 2, 3, 1, 3, 0]),
            int32_col(vec![2023, 2023, 2020, 2020, 2022, 2021, 2022]),
        ],
    )
    .unwrap();
    write_dataset(&out_dir, "rels_follows_flat.lance", schema, batch).await;

    // nodes_wrongversion: stamped v999 for the version-mismatch error test.
    let schema = schema_with_version(vec![Field::new("id", DataType::Int32, false)], "v999");
    let batch = RecordBatch::try_new(schema.clone(), vec![int32_col(vec![1])]).unwrap();
    write_dataset(&out_dir, "nodes_wrongversion.lance", schema, batch).await;
}
