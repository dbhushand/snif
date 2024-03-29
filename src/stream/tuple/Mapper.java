package stream.tuple;

import stream.AbstractPipe;

public class Mapper extends AbstractPipe<Tuple, Tuple> {

	int newType;
	int nrMappings;
	TupleAttribute[] fromAttributes;
	TupleAttribute[] toAttributes;
	
	public void process(Tuple o, int srcID, long timestamp) {
		Tuple newTuple = Tuple.createTuple(newType);
		for (int i=0; i< nrMappings ; i++) {
			newTuple.setAttribute( toAttributes[i], o.getAttribute(fromAttributes[i]));
		}
		transfer( newTuple, timestamp );

	}
	public Mapper(String newType, String... mapping) {
		nrMappings = mapping.length / 2;
		fromAttributes = new TupleAttribute[nrMappings];
		toAttributes =   new TupleAttribute[nrMappings];
		String[] toAttributesArray = new String [nrMappings];
		for (int i = 0; i < nrMappings; i++) {
			fromAttributes[i] = new TupleAttribute( mapping[i*2]); 
			toAttributes[i] =   new TupleAttribute( mapping[i*2+1] ); 
			toAttributesArray[i] = mapping[i*2+1];
		}
		Tuple.registerTupleType( newType, toAttributesArray);
		this.newType = Tuple.getTupleTypeID(newType);
	}
}
