/// Define test functions exercising an identifier type.
macro_rules! test_suite {
    ($type:ident, $mod:ident) => {
        mod $mod {
            use super::super::*;

            #[test]
            fn test_22_chars() {
                let _: $type = "6Oqc72NQoQpou9wXwmLZvG".try_into().expect("valid");
            }

            #[test]
            fn test_20_chars() {
                let _: $type = "qc72NQoQpou9wXwmLZvG".try_into().expect("valid");
            }

            #[test]
            fn test_1_char() {
                let _: $type = "q".try_into().expect("valid");
            }

            #[test]
            fn test_empty() {
                let r: Result<$type, _> = "".try_into();
                assert!(r.is_err());
                assert_eq!(r.err().unwrap().to_string(), "Empty Heavenly UUID");
            }

            #[test]
            fn test_too_long() {
                let r: Result<$type, _> = "X6Oqc72NQoQpou9wXwmLZvG".try_into();
                assert!(r.is_err());
                assert_eq!(
                    r.err().unwrap().to_string(),
                    "Heavenly UUID longer than the 22 character limit: \"X6Oqc72NQoQpou9wXwmLZvG\""
                );
            }

            #[test]
            fn test_bad_char() {
                let r: Result<$type, _> = "6 qc72NQoQpou9wXwmLZvG".try_into();
                assert!(r.is_err());
                assert_eq!(
                    r.err().unwrap().to_string(),
                    "Heavenly UUID contains a character outside of the base62 set: \"6 qc72NQoQpou9wXwmLZvG\""
                );
            }

            #[test]
            fn test_too_long_bad_char() {
                let r: Result<$type, _> = "X6 qc72NQoQpou9wXwmLZvG".try_into();
                assert!(r.is_err());
                assert_eq!(
                    r.err().unwrap().to_string(),
                    "Heavenly UUID longer than the 22 character limit: \"X6 qc72NQoQpou9wXwmLZvG\""
                );
            }

            #[test]
            fn test_bad_utf8() {
                // U+1F382 in UTF-8: F0 9F 8E 82
                // and in UTF-16: D83C DF82
                // The surrogate pair encoded in error as two UTF-8 sequences:
                let r: Result<$type, _> = b"\xed\xa0\xbc\xed\xbe\x82"[..].try_into();
                assert!(r.is_err());
                assert_eq!(
                    r.err().unwrap().to_string(),
                    "Heavenly UUID contains a character outside of the base62 set: ([237, 160, 188, 237, 190, 130]: invalid utf-8 sequence of 1 bytes from index 0)"
                );
            }

            #[test]
            fn test_from_static() {
                static SHORT: $type = $type::from_static("shortid");
                static TWENTYONE: $type = $type::from_static("almost22chaRSLong2121");
                static TWENTYTWO: $type = $type::from_static("testIDtestIDtestIDtest");

                for &(c, s) in &[
                    (&SHORT, "shortid"),
                    (&TWENTYONE, "almost22chaRSLong2121"),
                    (&TWENTYTWO, "testIDtestIDtestIDtest"),
                ] {
                    let runtime: $type = s.try_into().expect("legal id");
                    assert_eq!(c, &runtime, "static different from runtime for {:?}", s);
                    // ensure the whole struct is sound...
                    assert_eq!(c.b, runtime.b);
                    assert!((&c.b[c.len as usize..]).iter().all(|b| *b == 0));
                }
            }

            #[test]
            #[should_panic]
            fn test_static_error_invalid_char() {
                let _ = $type::from_static("invalid id");
            }

            #[test]
            #[should_panic]
            fn test_static_error_empty() {
                let _ = $type::from_static("");
            }

            #[test]
            #[should_panic]
            fn test_static_error_too_long() {
                let _ = $type::from_static("testIDtestIDtestIDtestID");
            }

            #[test]
            fn test_matches_input() {
                let s = "089asd8DjhkDSHKJF89";
                let res: $type = s.try_into().expect("valid");
                assert_eq!(res, s);
            }

            #[test]
            fn test_json() {
                const JSON: &str = "\"testIDtestIDtestIDtest\"";
                let obj: $type = "testIDtestIDtestIDtest".try_into().unwrap();
                let out = serde_json::to_string(&obj).unwrap();
                assert_eq!(JSON, out);

                let new_obj: $type = serde_json::from_str(JSON).unwrap();
                assert_eq!(new_obj, obj);
            }

            #[test]
            fn test_short_json() {
                const JSON: &str = "\"testIDtestIDtest\"";
                let obj: $type = "testIDtestIDtest".try_into().unwrap();
                let out = serde_json::to_string(&obj).unwrap();
                assert_eq!(JSON, out);

                let new_obj: $type = serde_json::from_str(JSON).unwrap();
                assert_eq!(new_obj, obj);
            }

            #[test]
            fn test_invalid_json() {
                for json in &[
                    "",
                    "\"\"",
                    "\"foo bar\"",                  // invalid char
                    "\"testIDtestIDtestIDtestID\"", // too long
                    // not bytes
                    "[116, 101, 115, 116, 73, 68, 116, 101, 115, 116, 73, 68, 116, 101, 115, 116, 73, 68, 116, 101, 115, 116]",
                    // nor the whole sequence as bytes
                    "[22, 116, 101, 115, 116, 73, 68, 116, 101, 115, 116, 73, 68, 116, 101, 115, 116, 73, 68, 116, 101, 115, 116]",
                ] {
                    assert!(serde_json::from_str::<$type>(json).is_err());
                }
            }

            #[test]
            fn test_bincode() {
                // 22-character long UUID
                const BINCODE: &[u8; 23] = b"\x16testIDtestIDtestIDtest";

                let obj: $type = "testIDtestIDtestIDtest".try_into().unwrap();
                let out = bincode::serialize(&obj).unwrap();
                assert_eq!(BINCODE, &*out);

                // Ensure our bincode scheme is in sync with the default...
                #[derive(Serialize, Deserialize, PartialEq, Debug)]
                struct Lookalike {
                    n: u8,
                    u: [u8; 22],
                }
                let look = Lookalike { n: 22, u: *b"testIDtestIDtestIDtest" };

                let la_bytes = bincode::serialize(&look).unwrap();
                assert_eq!(la_bytes, BINCODE);

                let new_la: Lookalike = bincode::deserialize(BINCODE).unwrap();
                assert_eq!(new_la, look);
            }

            #[test]
            fn test_short_bincode() {
                // 16-character long UUID
                const BINCODE: &[u8; 23] = b"\x10testIDtestIDtest\0\0\0\0\0\0";

                let obj: $type = "testIDtestIDtest".try_into().unwrap();
                let out = bincode::serialize(&obj).unwrap();
                assert_eq!(BINCODE, &*out);

                let new_obj: $type = bincode::deserialize(BINCODE).unwrap();
                assert_eq!(new_obj, obj);
            }

            #[test]
            fn test_invalid_bincode() {
                for bc in &[
                    b"" as &[u8],                                // no data, easy failure
                    b"\x10testIDtestIDtestIDtest",               // trailing non-zero bytes
                    b"\x10testIDtestIDtest",                     // message is too short
                    b"\x16\0\0\0\0\0\0\0testIDtestIDtestIDtest", // how a 22-byte &str would be encoded by default
                ] {
                    assert!(bincode::deserialize::<$type>(bc).is_err())
                }
            }
        }
    };
}

// Test the service, customer, and heavenly UUID types.
test_suite!(ServiceID, service_id);
test_suite!(CustomerID, customer_id);
test_suite!(HeavenlyUuid, heavenly_uuid);
