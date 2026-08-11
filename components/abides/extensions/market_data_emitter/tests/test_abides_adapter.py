from abides_market_data_emitter import split_safe_entry_id


def test_split_safe_entry_ids_are_non_zero_and_distinct() -> None:
    assert split_safe_entry_id(0, "VISIBLE") == 2
    assert split_safe_entry_id(0, "HIDDEN") == 3
    assert split_safe_entry_id(1, "VISIBLE") == 4
    assert split_safe_entry_id(1, "HIDDEN") == 5
