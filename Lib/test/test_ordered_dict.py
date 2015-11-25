import contextlib
import copy
import pickle
from random import randrange, shuffle
import sys
import unittest
from collections.abc import MutableMapping
from test import mapping_tests, support


py_coll = support.import_fresh_module('collections', blocked=['_collections'])
c_coll = support.import_fresh_module('collections', fresh=['_collections'])


@contextlib.contextmanager
def replaced_module(name, replacement):
    original_module = sys.modules[name]
    sys.modules[name] = replacement
    try:
        yield
    finally:
        sys.modules[name] = original_module


class OrderedDictTests:

    def test_init(self):
        OrderedDict = self.OrderedDict
        with self.assertRaises(TypeError):
            OrderedDict([('a', 1), ('b', 2)], None)                                 # too many args
        pairs = [('a', 1), ('b', 2), ('c', 3), ('d', 4), ('e', 5)]
        self.assertEqual(sorted(OrderedDict(dict(pairs)).items()), pairs)           # dict input
        self.assertEqual(sorted(OrderedDict(**dict(pairs)).items()), pairs)         # kwds input
        self.assertEqual(list(OrderedDict(pairs).items()), pairs)                   # pairs input
        self.assertEqual(list(OrderedDict([('a', 1), ('b', 2), ('c', 9), ('d', 4)],
                                          c=3, e=5).items()), pairs)                # mixed input

        # make sure no positional args conflict with possible kwdargs
        self.assertEqual(list(OrderedDict(self=42).items()), [('self', 42)])
        self.assertEqual(list(OrderedDict(other=42).items()), [('other', 42)])
        self.assertRaises(TypeError, OrderedDict, 42)
        self.assertRaises(TypeError, OrderedDict, (), ())
        self.assertRaises(TypeError, OrderedDict.__init__)

        # Make sure that direct calls to __init__ do not clear previous contents
        d = OrderedDict([('a', 1), ('b', 2), ('c', 3), ('d', 44), ('e', 55)])
        d.__init__([('e', 5), ('f', 6)], g=7, d=4)
        self.assertEqual(list(d.items()),
            [('a', 1), ('b', 2), ('c', 3), ('d', 4), ('e', 5), ('f', 6), ('g', 7)])

    def test_update(self):
        OrderedDict = self.OrderedDict
        with self.assertRaises(TypeError):
            OrderedDict().update([('a', 1), ('b', 2)], None)                        # too many args
        pairs = [('a', 1), ('b', 2), ('c', 3), ('d', 4), ('e', 5)]
        od = OrderedDict()
        od.update(dict(pairs))
        self.assertEqual(sorted(od.items()), pairs)                                 # dict input
        od = OrderedDict()
        od.update(**dict(pairs))
        self.assertEqual(sorted(od.items()), pairs)                                 # kwds input
        od = OrderedDict()
        od.update(pairs)
        self.assertEqual(list(od.items()), pairs)                                   # pairs input
        od = OrderedDict()
        od.update([('a', 1), ('b', 2), ('c', 9), ('d', 4)], c=3, e=5)
        self.assertEqual(list(od.items()), pairs)                                   # mixed input

        # Issue 9137: Named argument called 'other' or 'self'
        # shouldn't be treated specially.
        od = OrderedDict()
        od.update(self=23)
        self.assertEqual(list(od.items()), [('self', 23)])
        od = OrderedDict()
        od.update(other={})
        self.assertEqual(list(od.items()), [('other', {})])
        od = OrderedDict()
        od.update(red=5, blue=6, other=7, self=8)
        self.assertEqual(sorted(list(od.items())),
                         [('blue', 6), ('other', 7), ('red', 5), ('self', 8)])

        # Make sure that direct calls to update do not clear previous contents
        # add that updates items are not moved to the end
        d = OrderedDict([('a', 1), ('b', 2), ('c', 3), ('d', 44), ('e', 55)])
        d.update([('e', 5), ('f', 6)], g=7, d=4)
        self.assertEqual(list(d.items()),
            [('a', 1), ('b', 2), ('c', 3), ('d', 4), ('e', 5), ('f', 6), ('g', 7)])

        self.assertRaises(TypeError, OrderedDict().update, 42)
        self.assertRaises(TypeError, OrderedDict().update, (), ())
        self.assertRaises(TypeError, OrderedDict.update)

        self.assertRaises(TypeError, OrderedDict().update, 42)
        self.assertRaises(TypeError, OrderedDict().update, (), ())
        self.assertRaises(TypeError, OrderedDict.update)

    def test_fromkeys(self):
        OrderedDict = self.OrderedDict
        od = OrderedDict.fromkeys('abc')
        self.assertEqual(list(od.items()), [(c, None) for c in 'abc'])
        od = OrderedDict.fromkeys('abc', value=None)
        self.assertEqual(list(od.items()), [(c, None) for c in 'abc'])
        od = OrderedDict.fromkeys('abc', value=0)
        self.assertEqual(list(od.items()), [(c, 0) for c in 'abc'])

    def test_abc(self):
        OrderedDict = self.OrderedDict
        self.assertIsInstance(OrderedDict(), MutableMapping)
        self.assertTrue(issubclass(OrderedDict, MutableMapping))

    def test_clear(self):
        OrderedDict = self.OrderedDict
        pairs = [('c', 1), ('b', 2), ('a', 3), ('d', 4), ('e', 5), ('f', 6)]
        shuffle(pairs)
        od = OrderedDict(pairs)
        self.assertEqual(len(od), len(pairs))
        od.clear()
        self.assertEqual(len(od), 0)

    def test_delitem(self):
        OrderedDict = self.OrderedDict
        pairs = [('c', 1), ('b', 2), ('a', 3), ('d', 4), ('e', 5), ('f', 6)]
        od = OrderedDict(pairs)
        del od['a']
        self.assertNotIn('a', od)
        with self.assertRaises(KeyError):
            del od['a']
        self.assertEqual(list(od.items()), pairs[:2] + pairs[3:])

    def test_setitem(self):
        OrderedDict = self.OrderedDict
        od = OrderedDict([('d', 1), ('b', 2), ('c', 3), ('a', 4), ('e', 5)])
        od['c'] = 10           # existing element
        od['f'] = 20           # new element
        self.assertEqual(list(od.items()),
                         [('d', 1), ('b', 2), ('c', 10), ('a', 4), ('e', 5), ('f', 20)])

    def test_iterators(self):
        OrderedDict = self.OrderedDict
        pairs = [('c', 1), ('b', 2), ('a', 3), ('d', 4), ('e', 5), ('f', 6)]
        shuffle(pairs)
        od = OrderedDict(pairs)
        self.assertEqual(list(od), [t[0] for t in pairs])
        self.assertEqual(list(od.keys()), [t[0] for t in pairs])
        self.assertEqual(list(od.values()), [t[1] for t in pairs])
        self.assertEqual(list(od.items()), pairs)
        self.assertEqual(list(reversed(od)),
                         [t[0] for t in reversed(pairs)])
        self.assertEqual(list(reversed(od.keys())),
                         [t[0] for t in reversed(pairs)])
        self.assertEqual(list(reversed(od.values())),
                         [t[1] for t in reversed(pairs)])
        self.assertEqual(list(reversed(od.items())), list(reversed(pairs)))

    def test_detect_deletion_during_iteration(self):
        OrderedDict = self.OrderedDict
        od = OrderedDict.fromkeys('abc')
        it = iter(od)
        key = next(it)
        del od[key]
        with self.assertRaises(Exception):
            # Note, the exact exception raised is not guaranteed
            # The only guarantee that the next() will not succeed
            next(it)

    def test_sorted_iterators(self):
        OrderedDict = self.OrderedDict
        with self.assertRaises(TypeError):
            OrderedDict([('a', 1), ('b', 2)], None)
        pairs = [('a', 1), ('b', 2), ('c', 3), ('d', 4), ('e', 5)]
        od = OrderedDict(pairs)
        self.assertEqual(sorted(od), [t[0] for t in pairs])
        self.assertEqual(sorted(od.keys()), [t[0] for t in pairs])
        self.assertEqual(sorted(od.values()), [t[1] for t in pairs])
        self.assertEqual(sorted(od.items()), pairs)
        self.assertEqual(sorted(reversed(od)),
                         sorted([t[0] for t in reversed(pairs)]))

    def test_iterators_empty(self):
        OrderedDict = self.OrderedDict
        od = OrderedDict()
        empty = []
        self.assertEqual(list(od), empty)
        self.assertEqual(list(od.keys()), empty)
        self.assertEqual(list(od.values()), empty)
        self.assertEqual(list(od.items()), empty)
        self.assertEqual(list(reversed(od)), empty)
        self.assertEqual(list(reversed(od.keys())), empty)
        self.assertEqual(list(reversed(od.values())), empty)
        self.assertEqual(list(reversed(od.items())), empty)

    def test_popitem(self):
        OrderedDict = self.OrderedDict
        pairs = [('c', 1), ('b', 2), ('a', 3), ('d', 4), ('e', 5), ('f', 6)]
        shuffle(pairs)
        od = OrderedDict(pairs)
        while pairs:
            self.assertEqual(od.popitem(), pairs.pop())
        with self.assertRaises(KeyError):
            od.popitem()
        self.assertEqual(len(od), 0)

    def test_popitem_last(self):
        OrderedDict = self.OrderedDict
        pairs = [(i, i) for i in range(30)]

        obj = OrderedDict(pairs)
        for i in range(8):
            obj.popitem(True)
        obj.popitem(True)
        obj.popitem(last=True)
        self.assertEqual(len(obj), 20)

    def test_pop(self):
        OrderedDict = self.OrderedDict
        pairs = [('c', 1), ('b', 2), ('a', 3), ('d', 4), ('e', 5), ('f', 6)]
        shuffle(pairs)
        od = OrderedDict(pairs)
        shuffle(pairs)
        while pairs:
            k, v = pairs.pop()
            self.assertEqual(od.pop(k), v)
        with self.assertRaises(KeyError):
            od.pop('xyz')
        self.assertEqual(len(od), 0)
        self.assertEqual(od.pop(k, 12345), 12345)

        # make sure pop still works when __missing__ is defined
        class Missing(OrderedDict):
            def __missing__(self, key):
                return 0
        m = Missing(a=1)
        self.assertEqual(m.pop('b', 5), 5)
        self.assertEqual(m.pop('a', 6), 1)
        self.assertEqual(m.pop('a', 6), 6)
        self.assertEqual(m.pop('a', default=6), 6)
        with self.assertRaises(KeyError):
            m.pop('a')

    def test_equality(self):
        OrderedDict = self.OrderedDict
        pairs = [('c', 1), ('b', 2), ('a', 3), ('d', 4), ('e', 5), ('f', 6)]
        shuffle(pairs)
        od1 = OrderedDict(pairs)
        od2 = OrderedDict(pairs)
        self.assertEqual(od1, od2)          # same order implies equality
        pairs = pairs[2:] + pairs[:2]
        od2 = OrderedDict(pairs)
        self.assertNotEqual(od1, od2)       # different order implies inequality
        # comparison to regular dict is not order sensitive
        self.assertEqual(od1, dict(od2))
        self.assertEqual(dict(od2), od1)
        # different length implied inequality
        self.assertNotEqual(od1, OrderedDict(pairs[:-1]))

    def test_copying(self):
        OrderedDict = self.OrderedDict
        # Check that ordered dicts are copyable, deepcopyable, picklable,
        # and have a repr/eval round-trip
        pairs = [('c', 1), ('b', 2), ('a', 3), ('d', 4), ('e', 5), ('f', 6)]
        od = OrderedDict(pairs)
        def check(dup):
            msg = "\ncopy: %s\nod: %s" % (dup, od)
            self.assertIsNot(dup, od, msg)
            self.assertEqual(dup, od)
            self.assertEqual(list(dup.items()), list(od.items()))
            self.assertEqual(len(dup), len(od))
            self.assertEqual(type(dup), type(od))
        check(od.copy())
        check(copy.copy(od))
        check(copy.deepcopy(od))
        # pickle directly pulls the module, so we have to fake it
        with replaced_module('collections', self.module):
            for proto in range(pickle.HIGHEST_PROTOCOL + 1):
                with self.subTest(proto=proto):
                    check(pickle.loads(pickle.dumps(od, proto)))
        check(eval(repr(od)))
        update_test = OrderedDict()
        update_test.update(od)
        check(update_test)
        check(OrderedDict(od))

    def test_yaml_linkage(self):
        OrderedDict = self.OrderedDict
        # Verify that __reduce__ is setup in a way that supports PyYAML's dump() feature.
        # In yaml, lists are native but tuples are not.
        pairs = [('c', 1), ('b', 2), ('a', 3), ('d', 4), ('e', 5), ('f', 6)]
        od = OrderedDict(pairs)
        # yaml.dump(od) -->
        # '!!python/object/apply:__main__.OrderedDict\n- - [a, 1]\n  - [b, 2]\n'
        self.assertTrue(all(type(pair)==list for pair in od.__reduce__()[1]))

    def test_reduce_not_too_fat(self):
        OrderedDict = self.OrderedDict
        # do not save instance dictionary if not needed
        pairs = [('c', 1), ('b', 2), ('a', 3), ('d', 4), ('e', 5), ('f', 6)]
        od = OrderedDict(pairs)
        self.assertIsNone(od.__reduce__()[2])
        od.x = 10
        self.assertIsNotNone(od.__reduce__()[2])

    def test_pickle_recursive(self):
        OrderedDict = self.OrderedDict
        od = OrderedDict()
        od[1] = od

        # pickle directly pulls the module, so we have to fake it
        with replaced_module('collections', self.module):
            for proto in range(-1, pickle.HIGHEST_PROTOCOL + 1):
                dup = pickle.loads(pickle.dumps(od, proto))
                self.assertIsNot(dup, od)
                self.assertEqual(list(dup.keys()), [1])
                self.assertIs(dup[1], dup)

    def test_repr(self):
        OrderedDict = self.OrderedDict
        od = OrderedDict([('c', 1), ('b', 2), ('a', 3), ('d', 4), ('e', 5), ('f', 6)])
        self.assertEqual(repr(od),
            "OrderedDict([('c', 1), ('b', 2), ('a', 3), ('d', 4), ('e', 5), ('f', 6)])")
        self.assertEqual(eval(repr(od)), od)
        self.assertEqual(repr(OrderedDict()), "OrderedDict()")

    def test_repr_recursive(self):
        OrderedDict = self.OrderedDict
        # See issue #9826
        od = OrderedDict.fromkeys('abc')
        od['x'] = od
        self.assertEqual(repr(od),
            "OrderedDict([('a', None), ('b', None), ('c', None), ('x', ...)])")

    def test_setdefault(self):
        OrderedDict = self.OrderedDict
        pairs = [('c', 1), ('b', 2), ('a', 3), ('d', 4), ('e', 5), ('f', 6)]
        shuffle(pairs)
        od = OrderedDict(pairs)
        pair_order = list(od.items())
        self.assertEqual(od.setdefault('a', 10), 3)
        # make sure order didn't change
        self.assertEqual(list(od.items()), pair_order)
        self.assertEqual(od.setdefault('x', 10), 10)
        # make sure 'x' is added to the end
        self.assertEqual(list(od.items())[-1], ('x', 10))
        self.assertEqual(od.setdefault('g', default=9), 9)

        # make sure setdefault still works when __missing__ is defined
        class Missing(OrderedDict):
            def __missing__(self, key):
                return 0
        self.assertEqual(Missing().setdefault(5, 9), 9)

    def test_reinsert(self):
        OrderedDict = self.OrderedDict
        # Given insert a, insert b, delete a, re-insert a,
        # verify that a is now later than b.
        od = OrderedDict()
        od['a'] = 1
        od['b'] = 2
        del od['a']
        self.assertEqual(list(od.items()), [('b', 2)])
        od['a'] = 1
        self.assertEqual(list(od.items()), [('b', 2), ('a', 1)])

    def test_move_to_end(self):
        OrderedDict = self.OrderedDict
        od = OrderedDict.fromkeys('abcde')
        self.assertEqual(list(od), list('abcde'))
        od.move_to_end('c')
        self.assertEqual(list(od), list('abdec'))
        od.move_to_end('c', 0)
        self.assertEqual(list(od), list('cabde'))
        od.move_to_end('c', 0)
        self.assertEqual(list(od), list('cabde'))
        od.move_to_end('e')
        self.assertEqual(list(od), list('cabde'))
        od.move_to_end('b', last=False)
        self.assertEqual(list(od), list('bcade'))
        with self.assertRaises(KeyError):
            od.move_to_end('x')
        with self.assertRaises(KeyError):
            od.move_to_end('x', 0)

    def test_move_to_end_issue25406(self):
        OrderedDict = self.OrderedDict
        od = OrderedDict.fromkeys('abc')
        od.move_to_end('c', last=False)
        self.assertEqual(list(od), list('cab'))
        od.move_to_end('a', last=False)
        self.assertEqual(list(od), list('acb'))

        od = OrderedDict.fromkeys('abc')
        od.move_to_end('a')
        self.assertEqual(list(od), list('bca'))
        od.move_to_end('c')
        self.assertEqual(list(od), list('bac'))

    def test_sizeof(self):
        OrderedDict = self.OrderedDict
        # Wimpy test: Just verify the reported size is larger than a regular dict
        d = dict(a=1)
        od = OrderedDict(**d)
        self.assertGreater(sys.getsizeof(od), sys.getsizeof(d))

    def test_override_update(self):
        OrderedDict = self.OrderedDict
        # Verify that subclasses can override update() without breaking __init__()
        class MyOD(OrderedDict):
            def update(self, *args, **kwds):
                raise Exception()
        items = [('a', 1), ('c', 3), ('b', 2)]
        self.assertEqual(list(MyOD(items).items()), items)

    def test_highly_nested(self):
        # Issue 25395: crashes during garbage collection
        OrderedDict = self.OrderedDict
        obj = None
        for _ in range(1000):
            obj = OrderedDict([(None, obj)])
        del obj
        support.gc_collect()

    def test_highly_nested_subclass(self):
        # Issue 25395: crashes during garbage collection
        OrderedDict = self.OrderedDict
        deleted = []
        class MyOD(OrderedDict):
            def __del__(self):
                deleted.append(self.i)
        obj = None
        for i in range(100):
            obj = MyOD([(None, obj)])
            obj.i = i
        del obj
        support.gc_collect()
        self.assertEqual(deleted, list(reversed(range(100))))

    def test_delitem_hash_collision(self):
        OrderedDict = self.OrderedDict

        class Key:
            def __init__(self, hash):
                self._hash = hash
                self.value = str(id(self))
            def __hash__(self):
                return self._hash
            def __eq__(self, other):
                try:
                    return self.value == other.value
                except AttributeError:
                    return False
            def __repr__(self):
                return self.value

        def blocking_hash(hash):
            # See the collision-handling in lookdict (in Objects/dictobject.c).
            MINSIZE = 8
            i = (hash & MINSIZE-1)
            return (i << 2) + i + hash + 1

        COLLIDING = 1

        key = Key(COLLIDING)
        colliding = Key(COLLIDING)
        blocking = Key(blocking_hash(COLLIDING))

        od = OrderedDict()
        od[key] = ...
        od[blocking] = ...
        od[colliding] = ...
        od['after'] = ...

        del od[blocking]
        del od[colliding]
        self.assertEqual(list(od.items()), [(key, ...), ('after', ...)])

    def test_issue24347(self):
        OrderedDict = self.OrderedDict

        class Key:
            def __hash__(self):
                return randrange(100000)

        od = OrderedDict()
        for i in range(100):
            key = Key()
            od[key] = i

        # These should not crash.
        with self.assertRaises(KeyError):
            list(od.values())
        with self.assertRaises(KeyError):
            list(od.items())
        with self.assertRaises(KeyError):
            repr(od)
        with self.assertRaises(KeyError):
            od.copy()

    def test_issue24348(self):
        OrderedDict = self.OrderedDict

        class Key:
            def __hash__(self):
                return 1

        od = OrderedDict()
        od[Key()] = 0
        # This should not crash.
        od.popitem()

    def test_issue24667(self):
        """
        dict resizes after a certain number of insertion operations,
        whether or not there were deletions that freed up slots in the
        hash table.  During fast node lookup, OrderedDict must correctly
        respond to all resizes, even if the current "size" is the same
        as the old one.  We verify that here by forcing a dict resize
        on a sparse odict and then perform an operation that should
        trigger an odict resize (e.g. popitem).  One key aspect here is
        that we will keep the size of the odict the same at each popitem
        call.  This verifies that we handled the dict resize properly.
        """
        OrderedDict = self.OrderedDict

        od = OrderedDict()
        for c0 in '0123456789ABCDEF':
            for c1 in '0123456789ABCDEF':
                if len(od) == 4:
                    # This should not raise a KeyError.
                    od.popitem(last=False)
                key = c0 + c1
                od[key] = key

    # Direct use of dict methods

    def test_dict_setitem(self):
        OrderedDict = self.OrderedDict
        od = OrderedDict()
        dict.__setitem__(od, 'spam', 1)
        self.assertNotIn('NULL', repr(od))

    def test_dict_delitem(self):
        OrderedDict = self.OrderedDict
        od = OrderedDict()
        od['spam'] = 1
        od['ham'] = 2
        dict.__delitem__(od, 'spam')
        with self.assertRaises(KeyError):
            repr(od)

    def test_dict_clear(self):
        OrderedDict = self.OrderedDict
        od = OrderedDict()
        od['spam'] = 1
        od['ham'] = 2
        dict.clear(od)
        self.assertNotIn('NULL', repr(od))

    def test_dict_pop(self):
        OrderedDict = self.OrderedDict
        od = OrderedDict()
        od['spam'] = 1
        od['ham'] = 2
        dict.pop(od, 'spam')
        with self.assertRaises(KeyError):
            repr(od)

    def test_dict_popitem(self):
        OrderedDict = self.OrderedDict
        od = OrderedDict()
        od['spam'] = 1
        od['ham'] = 2
        dict.popitem(od)
        with self.assertRaises(KeyError):
            repr(od)

    def test_dict_setdefault(self):
        OrderedDict = self.OrderedDict
        od = OrderedDict()
        dict.setdefault(od, 'spam', 1)
        self.assertNotIn('NULL', repr(od))

    def test_dict_update(self):
        OrderedDict = self.OrderedDict
        od = OrderedDict()
        dict.update(od, [('spam', 1)])
        self.assertNotIn('NULL', repr(od))


class PurePythonOrderedDictTests(OrderedDictTests, unittest.TestCase):

    module = py_coll
    OrderedDict = py_coll.OrderedDict


@unittest.skipUnless(c_coll, 'requires the C version of the collections module')
class CPythonOrderedDictTests(OrderedDictTests, unittest.TestCase):

    module = c_coll
    OrderedDict = c_coll.OrderedDict

    def test_key_change_during_iteration(self):
        OrderedDict = self.OrderedDict

        od = OrderedDict.fromkeys('abcde')
        self.assertEqual(list(od), list('abcde'))
        with self.assertRaises(RuntimeError):
            for i, k in enumerate(od):
                od.move_to_end(k)
                self.assertLess(i, 5)
        with self.assertRaises(RuntimeError):
            for k in od:
                od['f'] = None
        with self.assertRaises(RuntimeError):
            for k in od:
                del od['c']
        self.assertEqual(list(od), list('bdeaf'))


class PurePythonOrderedDictSubclassTests(PurePythonOrderedDictTests):

    module = py_coll
    class OrderedDict(py_coll.OrderedDict):
        pass


class CPythonOrderedDictSubclassTests(CPythonOrderedDictTests):

    module = c_coll
    class OrderedDict(c_coll.OrderedDict):
        pass


class PurePythonGeneralMappingTests(mapping_tests.BasicTestMappingProtocol):

    @classmethod
    def setUpClass(cls):
        cls.type2test = py_coll.OrderedDict

    def test_popitem(self):
        d = self._empty_mapping()
        self.assertRaises(KeyError, d.popitem)


@unittest.skipUnless(c_coll, 'requires the C version of the collections module')
class CPythonGeneralMappingTests(mapping_tests.BasicTestMappingProtocol):

    @classmethod
    def setUpClass(cls):
        cls.type2test = c_coll.OrderedDict

    def test_popitem(self):
        d = self._empty_mapping()
        self.assertRaises(KeyError, d.popitem)


class PurePythonSubclassMappingTests(mapping_tests.BasicTestMappingProtocol):

    @classmethod
    def setUpClass(cls):
        class MyOrderedDict(py_coll.OrderedDict):
            pass
        cls.type2test = MyOrderedDict

    def test_popitem(self):
        d = self._empty_mapping()
        self.assertRaises(KeyError, d.popitem)


@unittest.skipUnless(c_coll, 'requires the C version of the collections module')
class CPythonSubclassMappingTests(mapping_tests.BasicTestMappingProtocol):

    @classmethod
    def setUpClass(cls):
        class MyOrderedDict(c_coll.OrderedDict):
            pass
        cls.type2test = MyOrderedDict

    def test_popitem(self):
        d = self._empty_mapping()
        self.assertRaises(KeyError, d.popitem)


if __name__ == "__main__":
    unittest.main()
