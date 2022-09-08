/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

mod abstract_environment_test {
    use sparta::datatype::AbstractDomain;
    use sparta::datatype::AbstractEnvironment;
    use sparta::datatype::HashMapAbstractEnvironment;
    use sparta::datatype::HashSetAbstractDomain;

    type Domain = HashSetAbstractDomain<String>;
    type Environment = HashMapAbstractEnvironment<String, Domain>;

    fn build_domain<const N: usize>(arr: [&str; N]) -> Domain {
        arr.iter().map(|str| str.to_string()).collect()
    }

    #[test]
    fn test_lattice_operations() {
        assert!(Environment::top().leq(&Environment::top()));
        assert!(!Environment::top().leq(&Environment::bottom()));
        assert!(Environment::bottom().leq(&Environment::top()));
        assert!(Environment::bottom().leq(&Environment::bottom()));

        assert!(Environment::bottom() == Environment::bottom());
        assert!(Environment::top() == Environment::top());
        assert!(Environment::bottom() != Environment::top());

        let mut e1 = Environment::top();
        e1.set("v1".to_owned(), build_domain(["a", "b"]));
        e1.set("v2".to_owned(), build_domain(["c"]));
        e1.set("v3".to_owned(), build_domain(["d", "e", "f"]));
        e1.set("v4".to_owned(), build_domain(["a", "f"]));
        assert_eq!(e1.len(), 4);

        let mut e2 = Environment::top();
        e2.set("v0".to_owned(), build_domain(["c", "f"]));
        e2.set("v2".to_owned(), build_domain(["c", "d"]));
        e2.set("v3".to_owned(), build_domain(["d", "e", "g", "h"]));
        assert_eq!(e2.len(), 3);

        let mut e3 = Environment::top();
        e3.set("v0".to_owned(), build_domain(["c", "d"]));
        e3.set("v2".to_owned(), Domain::bottom());
        e3.set("v3".to_owned(), build_domain(["a", "f", "g"]));
        assert!(e3.is_bottom());

        assert!(Environment::bottom().leq(&e1));
        assert!(!e1.leq(&Environment::bottom()));
        assert!(!Environment::top().leq(&e1));
        assert!(e1.leq(&Environment::top()));
        assert!(!e1.leq(&e2));
        assert!(!e2.leq(&e1));

        assert!(e1 == e1);
        assert!(e1 != e2);

        let join = e1.clone().join(e2.clone());
        assert!(e1.leq(&join));
        assert!(e2.leq(&join));
        assert_eq!(join.len(), 2);
        assert!(*join.get(&("v2".to_string())) == build_domain(["c", "d"]));
        assert!(*join.get(&("v3".to_string())) == build_domain(["d", "e", "f", "g", "h"]));

        assert!(*join.get(&("v4".to_string())) == Domain::top());

        assert!(e1.clone().join(Environment::top()).is_top());
        assert!(e1.clone().join(Environment::bottom()) == (e1));

        let meet = e1.clone().meet(e2.clone());
        assert!(meet.leq(&e1));
        assert!(meet.leq(&e2));
        assert_eq!(meet.len(), 5);

        assert!(*meet.get(&("v2".to_string())) == build_domain(["c"]));
        assert!(*meet.get(&("v3".to_string())) == build_domain(["d", "e"]));

        assert!(e1.clone().meet(Environment::bottom()).is_bottom());
        assert!(e1.clone().meet(Environment::top()) == e1);
    }
}
