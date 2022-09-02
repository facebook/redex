/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

mod abstract_partition_test {
    use std::collections::HashSet;

    use sparta::datatype::AbstractDomain;
    use sparta::datatype::AbstractPartition;
    use sparta::datatype::HashMapAbstractPartition;
    use sparta::datatype::HashSetAbstractDomain;

    type Domain = HashSetAbstractDomain<String>;
    type Partition = HashMapAbstractPartition<String, Domain>;

    fn build_domain<const N: usize>(arr: [&str; N]) -> Domain {
        arr.iter().map(|str| str.to_string()).collect()
    }

    #[test]
    fn test_lattice_operations() {
        assert!(Partition::top().leq(&Partition::top()));
        assert!(!Partition::top().leq(&Partition::bottom()));
        assert!(Partition::bottom().leq(&Partition::top()));
        assert!(Partition::bottom().leq(&Partition::bottom()));

        assert!(Partition::bottom() == Partition::bottom());
        assert!(Partition::top() == Partition::top());
        assert!(Partition::bottom() != Partition::top());

        let mut p1 = Partition::bottom();
        p1.set("v1".to_owned(), build_domain(["a", "b"]));
        p1.set("v2".to_owned(), build_domain(["c"]));
        p1.set("v3".to_owned(), build_domain(["d", "e", "f"]));
        p1.set("v4".to_owned(), build_domain(["a", "f"]));
        assert_eq!(p1.len(), 4);

        let mut p2 = Partition::bottom();
        p2.set("v0".to_owned(), build_domain(["c", "f"]));
        p2.set("v2".to_owned(), build_domain(["c", "d"]));
        p2.set("v3".to_owned(), build_domain(["d", "e", "g", "h"]));
        assert_eq!(p2.len(), 3);

        assert!(Partition::bottom().leq(&p1));
        assert!(!p1.leq(&Partition::bottom()));
        assert!(!Partition::top().leq(&p1));
        assert!(p1.leq(&Partition::top()));
        assert!(!p1.leq(&p2));
        assert!(!p2.leq(&p1));

        assert!(p1 == p1);
        assert!(p1 != p2);

        let join = p1.clone().join(p2.clone());
        assert!(p1.leq(&join));
        assert!(p2.leq(&join));
        assert_eq!(join.len(), 5);
        assert!(join.get(&("v0".to_string())) == p2.get(&("v0".to_string())));
        assert!(join.get(&("v1".to_string())) == p1.get(&("v1".to_string())));

        assert!(
            join.get(&("v2".to_string())).set()
                == &HashSet::from(["c".to_string(), "d".to_string()])
        );
        assert!(
            join.get(&("v3".to_string())).set()
                == &HashSet::from([
                    "d".to_string(),
                    "e".to_string(),
                    "f".to_string(),
                    "g".to_string(),
                    "h".to_string()
                ])
        );

        assert!(join.get(&("v4".to_string())) == p1.get(&("v4".to_string())));

        assert!(p1.clone().join(Partition::top()).is_top());
        assert!(p1.clone().join(Partition::bottom()) == (p1));

        let meet = p1.clone().meet(p2.clone());
        assert!(meet.leq(&p1));
        assert!(meet.leq(&p2));
        assert_eq!(meet.len(), 2);

        assert!(meet.get(&("v2".to_string())).set() == &HashSet::from(["c".to_string()]));
        assert!(
            meet.get(&("v3".to_string())).set()
                == &HashSet::from(["d".to_string(), "e".to_string()])
        );

        assert!(p1.clone().meet(Partition::bottom()).is_bottom());
        assert!(p1.clone().meet(Partition::top()) == p1);
    }
}
